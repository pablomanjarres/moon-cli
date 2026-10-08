#include "huffman.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK 65536U
#define MAX_WORKERS 8U
#define HEADER_SIZE (8U + 8U + 4U + 8U + 8U + 256U * 8U)
#define HASH_INITIAL UINT64_C(14695981039346656037)
#define HASH_PRIME UINT64_C(1099511628211)

struct Node {
    uint64_t frequency;
    int left, right, symbol;
};

struct Slot {
    unsigned char *data;
    uint32_t bytes, bits;
    int ready;
};

struct HuffmanJob {
    pthread_t coordinator;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    HuffmanStatus status;
    int events[2], input, snapshot, decompress;
    char *output;
    struct stat source;
    unsigned workers, window;
    uint64_t size, chunks, next, written, cursor, checksum;
    uint64_t frequencies[256];
    struct Node tree[511];
    unsigned char codes[256][256];
    unsigned short lengths[256], max_length;
    int root;
    struct Slot slots[MAX_WORKERS * 2];
};

static void notify_locked(HuffmanJob *job)
{
    unsigned char event = 1;
    ssize_t result;
    do result = write(job->events[1], &event, 1); while (result < 0 && errno == EINTR);
}

static int stopped_locked(HuffmanJob *job)
{
    return job->status.error || job->status.cancelled;
}

static int stopped(HuffmanJob *job)
{
    pthread_mutex_lock(&job->mutex);
    int result = stopped_locked(job);
    pthread_mutex_unlock(&job->mutex);
    return result;
}

static void fail_locked(HuffmanJob *job, int error)
{
    if (!stopped_locked(job)) job->status.error = error ? error : EIO;
    pthread_cond_broadcast(&job->changed);
    notify_locked(job);
}

static void fail(HuffmanJob *job, int error)
{
    pthread_mutex_lock(&job->mutex);
    fail_locked(job, error);
    pthread_mutex_unlock(&job->mutex);
}

static void progress(HuffmanJob *job, uint64_t bytes)
{
    pthread_mutex_lock(&job->mutex);
    job->status.completed += bytes;
    notify_locked(job);
    pthread_mutex_unlock(&job->mutex);
}

static int read_at(int fd, void *buffer, size_t bytes, uint64_t offset)
{
    unsigned char *p = buffer;
    while (bytes) {
        ssize_t n = pread(fd, p, bytes, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EINVAL; return -1; }
        p += n; bytes -= (size_t)n; offset += (uint64_t)n;
    }
    return 0;
}

static int write_all(int fd, const void *buffer, size_t bytes)
{
    const unsigned char *p = buffer;
    while (bytes) {
        ssize_t n = write(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        p += n; bytes -= (size_t)n;
    }
    return 0;
}

static uint64_t hash_bytes(uint64_t hash, const unsigned char *data, size_t bytes)
{
    for (size_t i = 0; i < bytes; ++i) hash = (hash ^ data[i]) * HASH_PRIME;
    return hash;
}

static void store_be(unsigned char *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = bytes; i; --i) { p[i - 1] = (unsigned char)value; value >>= 8; }
}

static uint64_t load_be(const unsigned char *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value = (value << 8) | p[i];
    return value;
}

static uint32_t chunk_bytes(HuffmanJob *job, uint64_t index)
{
    uint64_t remaining = job->size - index * CHUNK;
    return remaining < CHUNK ? (uint32_t)remaining : CHUNK;
}

static int snapshot_input(HuffmanJob *job)
{
    char temporary[] = "/tmp/moon-snapshot-XXXXXX";
    job->snapshot = mkstemp(temporary);
    if (job->snapshot < 0) return -1;
    if (unlink(temporary) < 0) return -1;
    unsigned char buffer[CHUNK];
    uint64_t size = (uint64_t)job->source.st_size, hash = HASH_INITIAL;
    for (uint64_t offset = 0; offset < size;) {
        if (stopped(job)) return -1;
        size_t bytes = size - offset < CHUNK ? (size_t)(size - offset) : CHUNK;
        if (read_at(job->input, buffer, bytes, offset) < 0 ||
            write_all(job->snapshot, buffer, bytes) < 0) return -1;
        hash = hash_bytes(hash, buffer, bytes);
        offset += bytes;
        progress(job, bytes);
    }
    struct stat after;
    if (fstat(job->input, &after) < 0) return -1;
#if defined(__APPLE__)
    int modified = after.st_mtimespec.tv_sec != job->source.st_mtimespec.tv_sec ||
                   after.st_mtimespec.tv_nsec != job->source.st_mtimespec.tv_nsec ||
                   after.st_ctimespec.tv_sec != job->source.st_ctimespec.tv_sec ||
                   after.st_ctimespec.tv_nsec != job->source.st_ctimespec.tv_nsec;
#else
    int modified = after.st_mtim.tv_sec != job->source.st_mtim.tv_sec ||
                   after.st_mtim.tv_nsec != job->source.st_mtim.tv_nsec ||
                   after.st_ctim.tv_sec != job->source.st_ctim.tv_sec ||
                   after.st_ctim.tv_nsec != job->source.st_ctim.tv_nsec;
#endif
    if (after.st_size != job->source.st_size || modified) { errno = EBUSY; return -1; }
    job->checksum = hash;
    close(job->input);
    job->input = -1;
    pthread_mutex_lock(&job->mutex);
    job->status.snapshot_ready = 1;
    notify_locked(job);
    pthread_mutex_unlock(&job->mutex);
    return 0;
}

static int build_tree(HuffmanJob *job)
{
    int active[511], count = 0;
    for (unsigned i = 0; i < 256; ++i) {
        if (!job->frequencies[i]) continue;
        job->tree[count] = (struct Node){job->frequencies[i], -1, -1, (int)i};
        active[count] = 1;
        ++count;
    }
    int remaining = count;
    while (remaining > 1) {
        int a = -1, b = -1;
        for (int i = 0; i < count; ++i) {
            if (!active[i]) continue;
            if (a < 0 || job->tree[i].frequency < job->tree[a].frequency) { b = a; a = i; }
            else if (b < 0 || job->tree[i].frequency < job->tree[b].frequency) b = i;
        }
        job->tree[count] = (struct Node){job->tree[a].frequency + job->tree[b].frequency, a, b, -1};
        active[a] = active[b] = 0;
        active[count++] = 1;
        --remaining;
    }
    job->root = count - 1;
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        if (!job->frequencies[symbol]) continue;
        int leaf = 0;
        while (job->tree[leaf].symbol != (int)symbol) ++leaf;
        unsigned length = 0;
        unsigned char reverse[256];
        int child = leaf;
        while (child != job->root) {
            int parent = child + 1;
            while (job->tree[parent].left != child && job->tree[parent].right != child) ++parent;
            reverse[length++] = job->tree[parent].right == child;
            child = parent;
        }
        if (!length) reverse[length++] = 0;
        job->lengths[symbol] = (unsigned short)length;
        if (length > job->max_length) job->max_length = (unsigned short)length;
        for (unsigned i = 0; i < length; ++i) job->codes[symbol][i] = reverse[length - i - 1];
    }
    return 0;
}

static void *count_worker(void *argument)
{
    HuffmanJob *job = argument;
    uint64_t local[256] = {0};
    unsigned char buffer[CHUNK];
    for (;;) {
        pthread_mutex_lock(&job->mutex);
        uint64_t index = job->next;
        int finish = stopped_locked(job) || index == job->chunks;
        if (!finish) ++job->next;
        pthread_mutex_unlock(&job->mutex);
        if (finish) break;
        uint32_t bytes = chunk_bytes(job, index);
        if (read_at(job->snapshot, buffer, bytes, index * CHUNK) < 0) { fail(job, errno); break; }
        for (uint32_t i = 0; i < bytes; ++i) ++local[buffer[i]];
        progress(job, bytes);
    }
    pthread_mutex_lock(&job->mutex);
    for (unsigned i = 0; i < 256; ++i) job->frequencies[i] += local[i];
    pthread_mutex_unlock(&job->mutex);
    return NULL;
}

static int read_chunk_locked(HuffmanJob *job, uint64_t index, uint64_t *offset,
                             uint32_t *bits)
{
    unsigned char header[8];
    uint64_t input_size = (uint64_t)job->source.st_size;
    if (job->cursor > input_size || input_size - job->cursor < sizeof header) {
        errno = EINVAL; return -1;
    }
    if (read_at(job->snapshot, header, sizeof header, job->cursor) < 0) return -1;
    uint32_t bytes = chunk_bytes(job, index);
    *bits = (uint32_t)load_be(header + 4, 4);
    uint64_t payload = ((uint64_t)*bits + 7) / 8;
    if (load_be(header, 4) != bytes || *bits < bytes ||
        *bits > (uint64_t)bytes * job->max_length ||
        payload > input_size - job->cursor - sizeof header) { errno = EINVAL; return -1; }
    *offset = job->cursor + sizeof header;
    job->cursor = *offset + payload;
    return 0;
}

static unsigned char *encode_chunk(HuffmanJob *job, uint64_t index, uint32_t *bits)
{
    unsigned char input[CHUNK];
    uint32_t bytes = chunk_bytes(job, index);
    if (read_at(job->snapshot, input, bytes, index * CHUNK) < 0) return NULL;
    *bits = 0;
    for (uint32_t i = 0; i < bytes; ++i) *bits += job->lengths[input[i]];
    unsigned char *output = calloc(((size_t)*bits + 7) / 8, 1);
    if (!output) return NULL;
    uint32_t position = 0;
    for (uint32_t i = 0; i < bytes; ++i) {
        if (!(i % 4096) && stopped(job)) { free(output); errno = ECANCELED; return NULL; }
        unsigned symbol = input[i];
        for (unsigned bit = 0; bit < job->lengths[symbol]; ++bit, ++position)
            output[position / 8] |= job->codes[symbol][bit] << (7 - position % 8);
    }
    return output;
}

static unsigned char *decode_chunk(HuffmanJob *job, uint64_t index, uint64_t offset,
                                  uint32_t bits)
{
    uint32_t bytes = chunk_bytes(job, index);
    size_t payload = ((size_t)bits + 7) / 8;
    unsigned char *input = malloc(payload), *output = malloc(bytes);
    if (!input || !output) { free(input); free(output); return NULL; }
    if (read_at(job->snapshot, input, payload, offset) < 0) goto invalid;
    if ((bits % 8) && (input[payload - 1] & ((1U << (8 - bits % 8)) - 1))) {
        errno = EINVAL; goto invalid;
    }
    uint32_t written = 0;
    int node = job->root;
    for (uint32_t i = 0; i < bits; ++i) {
        if (!(i % 32768) && stopped(job)) { errno = ECANCELED; goto invalid; }
        unsigned bit = (input[i / 8] >> (7 - i % 8)) & 1;
        if (job->tree[job->root].symbol >= 0) {
            if (bit) { errno = EINVAL; goto invalid; }
        } else node = bit ? job->tree[node].right : job->tree[node].left;
        if (job->tree[node].symbol >= 0) {
            if (written == bytes) { errno = EINVAL; goto invalid; }
            output[written++] = (unsigned char)job->tree[node].symbol;
            node = job->root;
        }
    }
    if (written != bytes || node != job->root) { errno = EINVAL; goto invalid; }
    free(input);
    return output;
invalid:
    free(input); free(output);
    return NULL;
}

static void *codec_worker(void *argument)
{
    HuffmanJob *job = argument;
    for (;;) {
        uint64_t offset = 0;
        uint32_t bits = 0;
        pthread_mutex_lock(&job->mutex);
        while (!stopped_locked(job) && job->next < job->chunks &&
               job->next - job->written >= job->window)
            pthread_cond_wait(&job->changed, &job->mutex);
        uint64_t index = job->next;
        if (stopped_locked(job) || index == job->chunks) {
            pthread_mutex_unlock(&job->mutex); break;
        }
        if (job->decompress && read_chunk_locked(job, index, &offset, &bits) < 0) {
            fail_locked(job, errno);
            pthread_mutex_unlock(&job->mutex); break;
        }
        ++job->next;
        pthread_mutex_unlock(&job->mutex);
        unsigned char *data = job->decompress ? decode_chunk(job, index, offset, bits)
                                             : encode_chunk(job, index, &bits);
        if (!data) { fail(job, errno); break; }
        pthread_mutex_lock(&job->mutex);
        job->slots[index % job->window] = (struct Slot){data, chunk_bytes(job, index), bits, 1};
        pthread_cond_broadcast(&job->changed);
        pthread_mutex_unlock(&job->mutex);
    }
    return NULL;
}

static unsigned start_workers(HuffmanJob *job, pthread_t *threads, void *(*worker)(void *))
{
    unsigned count = 0;
    job->next = job->written = 0;
    for (; count < job->workers; ++count) {
        int error = pthread_create(&threads[count], NULL, worker, job);
        if (error) { fail(job, error); break; }
    }
    return count;
}

static void join_workers(pthread_t *threads, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) pthread_join(threads[i], NULL);
}

static int archive_header(HuffmanJob *job, int output)
{
    unsigned char header[HEADER_SIZE];
    if (job->decompress) {
        if (job->source.st_size < HEADER_SIZE) { errno = EINVAL; return -1; }
        if (read_at(job->snapshot, header, sizeof header, 0) < 0) return -1;
        job->size = load_be(header + 8, 8);
        job->chunks = load_be(header + 20, 8);
        job->checksum = load_be(header + 28, 8);
        if (memcmp(header, "MOONHF01", 8) || load_be(header + 16, 4) != CHUNK ||
            job->size > (uint64_t)LLONG_MAX - (uint64_t)job->source.st_size ||
            job->chunks != job->size / CHUNK + (job->size % CHUNK != 0) ||
            job->chunks > ((uint64_t)job->source.st_size - HEADER_SIZE) / 9) {
            errno = EINVAL; return -1;
        }
        uint64_t total = 0;
        for (unsigned i = 0; i < 256; ++i) {
            job->frequencies[i] = load_be(header + 36 + i * 8, 8);
            if (job->frequencies[i] > job->size - total) { errno = EINVAL; return -1; }
            total += job->frequencies[i];
        }
        if (total != job->size) { errno = EINVAL; return -1; }
        job->cursor = HEADER_SIZE;
        pthread_mutex_lock(&job->mutex);
        job->status.total = (uint64_t)job->source.st_size + job->size;
        notify_locked(job);
        pthread_mutex_unlock(&job->mutex);
        return 0;
    }
    memcpy(header, "MOONHF01", 8);
    store_be(header + 8, job->size, 8);
    store_be(header + 16, CHUNK, 4);
    store_be(header + 20, job->chunks, 8);
    store_be(header + 28, job->checksum, 8);
    for (unsigned i = 0; i < 256; ++i) store_be(header + 36 + i * 8, job->frequencies[i], 8);
    return write_all(output, header, sizeof header);
}

static int write_chunks(HuffmanJob *job, int output)
{
    pthread_t threads[MAX_WORKERS];
    unsigned count = start_workers(job, threads, codec_worker);
    uint64_t hash = HASH_INITIAL, frequencies[256] = {0};
    while (job->written < job->chunks) {
        pthread_mutex_lock(&job->mutex);
        struct Slot *slot = &job->slots[job->written % job->window];
        while (!slot->ready && !stopped_locked(job)) pthread_cond_wait(&job->changed, &job->mutex);
        if (stopped_locked(job)) { pthread_mutex_unlock(&job->mutex); break; }
        struct Slot chunk = *slot;
        pthread_mutex_unlock(&job->mutex);
        int error = 0;
        if (job->decompress) {
            if (write_all(output, chunk.data, chunk.bytes) < 0) error = errno;
            hash = hash_bytes(hash, chunk.data, chunk.bytes);
            for (uint32_t i = 0; i < chunk.bytes; ++i) ++frequencies[chunk.data[i]];
        } else {
            unsigned char header[8];
            store_be(header, chunk.bytes, 4); store_be(header + 4, chunk.bits, 4);
            if (write_all(output, header, sizeof header) < 0 ||
                write_all(output, chunk.data, ((size_t)chunk.bits + 7) / 8) < 0) error = errno;
        }
        pthread_mutex_lock(&job->mutex);
        free(chunk.data);
        memset(slot, 0, sizeof *slot);
        ++job->written;
        job->status.completed += chunk.bytes;
        if (error) fail_locked(job, error);
        pthread_cond_broadcast(&job->changed);
        notify_locked(job);
        pthread_mutex_unlock(&job->mutex);
    }
    join_workers(threads, count);
    if (stopped(job)) return -1;
    if (job->decompress && (job->cursor != (uint64_t)job->source.st_size ||
        hash != job->checksum || memcmp(frequencies, job->frequencies, sizeof frequencies))) {
        errno = EINVAL; return -1;
    }
    return 0;
}

