#include "huffman.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
