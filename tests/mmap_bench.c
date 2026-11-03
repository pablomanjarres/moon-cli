/*
 * mmap_bench.c — cost of mmap() + first page fault + munmap(), with or without the guard.
 *
 *   ./mmap_bench <guarded file> <other file> [iterations]
 *
 * Each iteration maps one page read-only, touches it (one minor page fault) and
 * unmaps it. The guarded file pays the full probe path; the other file pays only the
 * inode comparison every mmap() now goes through.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

static void bench(const char *label, const char *path, long iters)
{
    int fd = open(path, O_RDONLY);
    if (fd == -1) { perror(path); exit(1); }

    struct rusage before, after;
    volatile char sink = 0;
    getrusage(RUSAGE_SELF, &before);
    double start = now_ns();
    for (long i = 0; i < iters; i++) {
        char *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { perror("mmap"); exit(1); }
        sink += m[0];
        munmap(m, 4096);
    }
    double ns = (now_ns() - start) / iters;
    getrusage(RUSAGE_SELF, &after);
    close(fd);
    (void)sink;

    printf("  %-8s %8.0f ns/iter   %ld minor faults\n",
           label, ns, after.ru_minflt - before.ru_minflt);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <guarded> <other> [iterations]\n", argv[0]); return 2; }
    long iters = argc > 3 ? atol(argv[3]) : 200000;

    bench("warmup", argv[2], iters / 10);
    bench("guarded", argv[1], iters);
    bench("other", argv[2], iters);
    return 0;
}
