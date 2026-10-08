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

