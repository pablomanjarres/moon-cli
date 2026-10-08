#include "huffman.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static ssize_t held_write(int fd, const void *data, size_t bytes);
#define write held_write
#include "../huffman.c"
#undef write

static pthread_mutex_t write_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t write_changed = PTHREAD_COND_INITIALIZER;
static size_t held_bytes;
static int write_reached;

static ssize_t held_write(int fd, const void *data, size_t bytes)
{
    pthread_mutex_lock(&write_mutex);
    if (held_bytes && bytes == held_bytes) {
        write_reached = 1;
        pthread_cond_broadcast(&write_changed);
        while (held_bytes) pthread_cond_wait(&write_changed, &write_mutex);
    }
    pthread_mutex_unlock(&write_mutex);
    return write(fd, data, bytes);
}

static void hold_write(size_t bytes)
{
    pthread_mutex_lock(&write_mutex);
    held_bytes = bytes;
    write_reached = 0;
    pthread_cond_broadcast(&write_changed);
    pthread_mutex_unlock(&write_mutex);
}

static void wait_write(void)
{
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 10;
    pthread_mutex_lock(&write_mutex);
    while (!write_reached)
        assert(pthread_cond_timedwait(&write_changed, &write_mutex, &deadline) == 0);
    pthread_mutex_unlock(&write_mutex);
}

static char directory[] = "/tmp/moon-huffman-test-XXXXXX";
static unsigned serial;

static void path(char *out, const char *name)
{
    snprintf(out, 512, "%s/%u-%s", directory, serial++, name);
}

static void put(const char *name, const unsigned char *data, size_t size)
{
    int fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    while (size) {
        ssize_t n = write(fd, data, size);
        assert(n > 0);
        data += n;
        size -= (size_t)n;
    }
    assert(close(fd) == 0);
}

static HuffmanStatus wait_job(HuffmanJob *job)
{
    struct pollfd event = {huffman_event_fd(job), POLLIN, 0};
    HuffmanStatus state;
    unsigned long long previous = 0;
    do {
        state = huffman_status(job);
        assert(state.completed >= previous && state.completed <= state.total);
        previous = state.completed;
        if (!state.done) assert(poll(&event, 1, 10000) > 0);
    } while (!state.done);
    return state;
}

static HuffmanStatus run(const char *input, const char *output, int decompress,
                         unsigned workers)
{
    int fd = open(input, O_RDONLY);
    assert(fd >= 0);
    HuffmanJob *job = huffman_start(fd, output, decompress, workers);
    assert(job);
    close(fd);
    HuffmanStatus state = wait_job(job);
    huffman_cancel(job);
    HuffmanStatus after_cancel = huffman_status(job);
    assert(after_cancel.done && after_cancel.cancelled == state.cancelled);
    huffman_destroy(job);
    return state;
}

static void equal_files(const char *left, const char *right)
{
    int a = open(left, O_RDONLY), b = open(right, O_RDONLY);
    unsigned char x[4096], y[4096];
    assert(a >= 0 && b >= 0);
    for (;;) {
        ssize_t nx = read(a, x, sizeof x), ny = read(b, y, sizeof y);
        assert(nx >= 0 && nx == ny && memcmp(x, y, (size_t)nx) == 0);
        if (!nx) break;
    }
    close(a);
    close(b);
}

static void roundtrip(const unsigned char *data, size_t size)
{
    char input[512], compressed[512], second[512], output[512];
    path(input, "input"); path(compressed, "compressed");
    path(second, "second"); path(output, "output");
    put(input, data, size);
    HuffmanStatus state = run(input, compressed, 0, 4);
    assert(!state.error && !state.cancelled && state.snapshot_ready);
    assert(state.completed == state.total);
    state = run(compressed, output, 1, 3);
    assert(!state.error && !state.cancelled && state.snapshot_ready);
    equal_files(input, output);
    assert(!run(input, second, 0, 1).error);
    equal_files(compressed, second);
    unlink(input); unlink(compressed); unlink(second); unlink(output);
}

static void invalid_archives(void)
{
    unsigned char data[65539];
    memset(data, 'a', sizeof data);
    char input[512], archive[512], output[512];
    path(input, "input"); path(archive, "archive"); path(output, "output");
    put(input, data, sizeof data);
    assert(!run(input, archive, 0, 8).error);
    int fd = open(archive, O_RDWR);
    struct stat info;
    assert(fd >= 0 && fstat(fd, &info) == 0);
    unsigned char saved;
    const off_t fields[] = {0, 15, 19, 27, 28, 36, 2084};
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
        assert(pread(fd, &saved, 1, fields[i]) == 1);
        unsigned char damaged = saved ^ 1;
        assert(pwrite(fd, &damaged, 1, fields[i]) == 1);
        assert(run(archive, output, 1, 2).error == EINVAL);
        assert(access(output, F_OK) < 0 && errno == ENOENT);
        assert(pwrite(fd, &saved, 1, fields[i]) == 1);
    }
    unsigned char last;
    assert(pread(fd, &last, 1, info.st_size - 1) == 1);
    last ^= 0x80;
    assert(pwrite(fd, &last, 1, info.st_size - 1) == 1);
    assert(run(archive, output, 1, 2).error == EINVAL);
    assert(access(output, F_OK) < 0 && errno == ENOENT);
    assert(ftruncate(fd, info.st_size - 1) == 0);
    close(fd);
    assert(run(archive, output, 1, 2).error == EINVAL);
    unlink(input); unlink(archive);
}

static void snapshot_and_destination(void)
{
    size_t size = 2 * 1024 * 1024;
    unsigned char *data = malloc(size);
    assert(data);
    memset(data, 's', size);
    char input[512], expected[512], archive[512], output[512];
    path(input, "source"); path(expected, "expected");
    path(archive, "archive"); path(output, "restored");
    put(input, data, size); put(expected, data, size);
    int fd = open(input, O_RDWR);
    assert(fd >= 0);
    HuffmanJob *job = huffman_start(fd, archive, 0, 4);
    assert(job);
    struct pollfd event = {huffman_event_fd(job), POLLIN, 0};
    while (!huffman_status(job).snapshot_ready) assert(poll(&event, 1, 10000) > 0);
    unsigned char edited = 'x';
    assert(pwrite(fd, &edited, 1, 0) == 1);
    assert(!wait_job(job).error);
    huffman_destroy(job);
    assert(!run(archive, output, 1, 4).error);
    equal_files(expected, output);
    unlink(archive);
    hold_write(HEADER_SIZE);
    job = huffman_start(fd, archive, 0, 4);
    assert(job);
    wait_write();
    put(archive, &edited, 1);
    hold_write(0);
    assert(wait_job(job).error == EEXIST);
    huffman_destroy(job);
    int existing = open(archive, O_RDONLY);
    unsigned char check;
    assert(existing >= 0 && read(existing, &check, 1) == 1 && check == edited);
    close(existing);
    close(fd);
    free(data);
    unlink(input); unlink(expected); unlink(archive); unlink(output);
}

static void lifecycle(void)
{
    unsigned char data[65536];
    memset(data, 7, sizeof data);
    char input[512], output[512];
    path(input, "input"); path(output, "output");
    put(input, data, sizeof data);
    int fd = open(input, O_RDWR);
    assert(fd >= 0);
    errno = 0;
    assert(!huffman_start(fd, input, 0, 4) && errno == EEXIST);
    int pipes[2];
    assert(pipe(pipes) == 0);
    errno = 0;
    assert(!huffman_start(pipes[0], output, 0, 4) && errno == EINVAL);
    close(pipes[0]); close(pipes[1]);
    for (int i = 0; i < 20; ++i) {
        hold_write(HEADER_SIZE);
        HuffmanJob *job = huffman_start(fd, output, 0, 4);
        assert(job);
        huffman_cancel(job);
        hold_write(0);
        HuffmanStatus state = wait_job(job);
        assert(state.cancelled && !state.error);
        huffman_destroy(job);
        assert(access(output, F_OK) < 0 && errno == ENOENT);
    }
    size_t large = 16 * CHUNK;
    assert(ftruncate(fd, (off_t)large) == 0);
    hold_write(CHUNK / 8);
    HuffmanJob *job = huffman_start(fd, output, 0, 4);
    assert(job);
    wait_write();
    HuffmanStatus state = huffman_status(job);
    assert(state.snapshot_ready && state.completed == 2 * large);
    assert(!state.error && !state.done);
    huffman_cancel(job);
    hold_write(0);
    state = wait_job(job);
    assert(state.cancelled && !state.error);
    huffman_destroy(job);
    assert(access(output, F_OK) < 0 && errno == ENOENT);
    close(fd); unlink(input);
}

int main(void)
{
    assert(mkdtemp(directory));
    roundtrip((const unsigned char *)"", 0);
    roundtrip((const unsigned char *)"one line without a final newline", 32);
    unsigned char single[70000];
    memset(single, 0, sizeof single);
    roundtrip(single, sizeof single);
    unsigned char skewed[65535];
    size_t position = 0;
    for (unsigned symbol = 0; symbol < 16; ++symbol)
        for (unsigned i = 0; i < (1U << symbol); ++i) skewed[position++] = (unsigned char)symbol;
    roundtrip(skewed, position);
    size_t size = 3 * 1024 * 1024 + 17;
    unsigned char *binary = malloc(size);
    assert(binary);
    uint32_t seed = 17;
    for (size_t i = 0; i < size; ++i) {
        seed = seed * 1664525U + 1013904223U;
        binary[i] = (unsigned char)(seed >> 24);
    }
    roundtrip(binary, size);
    free(binary);
    invalid_archives();
    snapshot_and_destination();
    lifecycle();
    assert(rmdir(directory) == 0);
    puts("Huffman roundtrip, determinism, corruption, cancellation: passed");
    return 0;
}
