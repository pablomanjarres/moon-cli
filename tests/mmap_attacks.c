/*
 * mmap_attacks.c — tries to modify or execute a protected file through mmap().
 *
 *   ./mmap_attacks <file> guarded   every attack must fail and the file must not change
 *   ./mmap_attacks <file> open      no module loaded: the same shared write succeeds
 *
 * Runs as an ordinary user who owns the file and has write permission on it, so any
 * refusal comes from kernel/mmap_guard.ko and not from POSIX permissions.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *path;
static char original[4096];
static size_t len;
static int failures;

static void check(const char *name, int ok)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) failures++;
}

/* Re-reads the file with read(2) and compares it with the copy taken at start. */
static int file_intact(void)
{
    char now[sizeof(original)];
    int fd = open(path, O_RDONLY);
    ssize_t got = fd == -1 ? -1 : read(fd, now, sizeof(now));
    if (fd != -1) close(fd);
    return got == (ssize_t)len && memcmp(now, original, len) == 0;
}

/* mmap() that reports errno instead of MAP_FAILED. */
static int try_map(int oflag, int prot, int flags, char **out)
{
    int fd = open(path, oflag);
    if (fd == -1) return errno;
    char *m = mmap(NULL, len, prot, flags, fd, 0);
    int err = m == MAP_FAILED ? errno : 0;
    close(fd);
    *out = m == MAP_FAILED ? NULL : m;
    return err;
}

/* ------------------------------------------------------------------------------------
 * Dirty COW pattern (CVE-2016-5195): one thread keeps dropping the private COW page
 * with madvise(MADV_DONTNEED) while another writes to the read-only mapping through
 * /proc/self/mem. On a vulnerable kernel the write lands in the page cache.
 * ------------------------------------------------------------------------------------ */
static char *race_map;
static volatile int race_stop;

static void *race_madvise(void *arg)
{
    (void)arg;
    while (!race_stop) madvise(race_map, len, MADV_DONTNEED);
    return NULL;
}

static void *race_procmem(void *arg)
{
    (void)arg;
    int fd = open("/proc/self/mem", O_RDWR);
    if (fd == -1) return NULL;
    while (!race_stop) pwrite(fd, "PWND", 4, (off_t)(unsigned long)race_map);
    close(fd);
    return NULL;
}

static void dirty_cow_race(double seconds)
{
    if (try_map(O_RDONLY, PROT_READ, MAP_PRIVATE, &race_map)) {
        check("race: read-only private mapping", 0);
        return;
    }
    pthread_t a, b;
    race_stop = 0;
    pthread_create(&a, NULL, race_madvise, NULL);
    pthread_create(&b, NULL, race_procmem, NULL);
    struct timespec t = { (time_t)seconds, (long)((seconds - (time_t)seconds) * 1e9) };
    nanosleep(&t, NULL);
    race_stop = 1;
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    munmap(race_map, len);
    check("race: madvise + /proc/self/mem leaves the file intact", file_intact());
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s <file> guarded|open\n", argv[0]); return 2; }
    path = argv[1];
    int guarded = strcmp(argv[2], "guarded") == 0;

    int fd = open(path, O_RDONLY);
    ssize_t got = fd == -1 ? -1 : read(fd, original, sizeof(original));
    if (fd != -1) close(fd);
    if (got <= 0) { perror(path); return 2; }
    len = (size_t)got;
    check("POSIX permissions allow this user to write the file", access(path, W_OK) == 0);

    char *m;
    int err;

    if (!guarded) {
        err = try_map(O_RDWR, PROT_READ | PROT_WRITE, MAP_SHARED, &m);
        check("without the module a shared writable mapping succeeds", err == 0);
        if (m) munmap(m, len);
        return failures ? 1 : 0;
    }

    /* 1. Read in place: allowed, and the bytes match read(2). */
    err = try_map(O_RDONLY, PROT_READ, MAP_SHARED, &m);
    check("read-only shared mapping is allowed", err == 0);
    if (m) { check("mapped bytes match the file", memcmp(m, original, len) == 0); munmap(m, len); }

    /* 2. The attack the module exists for: write the file through a shared map. */
    err = try_map(O_RDWR, PROT_READ | PROT_WRITE, MAP_SHARED, &m);
    check("shared writable mapping is refused with EACCES", err == EACCES);
    if (m) { memcpy(m, "PWND", 4); msync(m, len, MS_SYNC); munmap(m, len); }

    /* 3. Same, through MAP_SHARED_VALIDATE. */
#ifdef MAP_SHARED_VALIDATE
    err = try_map(O_RDWR, PROT_READ | PROT_WRITE, MAP_SHARED_VALIDATE, &m);
    check("MAP_SHARED_VALIDATE writable mapping is refused", err == EACCES);
    if (m) munmap(m, len);
#endif

    /* 4. Map read-only, then try to widen it with mprotect(). */
    err = try_map(O_RDWR, PROT_READ, MAP_SHARED, &m);
    check("read-only shared mapping on an O_RDWR descriptor is allowed", err == 0);
    if (m) {
        int r = mprotect(m, len, PROT_READ | PROT_WRITE);
        check("mprotect() up to PROT_WRITE is refused with EACCES", r == -1 && errno == EACCES);
        munmap(m, len);
    }

    /* 5. The file as code. */
    err = try_map(O_RDONLY, PROT_READ | PROT_EXEC, MAP_PRIVATE, &m);
    check("executable mapping is refused with EACCES", err == EACCES);
    if (m) munmap(m, len);

    /* 6. A private copy is allowed: copy-on-write keeps the file untouched. */
    err = try_map(O_RDONLY, PROT_READ | PROT_WRITE, MAP_PRIVATE, &m);
    check("private writable mapping (COW) is allowed", err == 0);
    if (m) { memcpy(m, "PWND", 4); munmap(m, len); }

    /* 7. Private map, then mprotect to exec. */
    err = try_map(O_RDONLY, PROT_READ, MAP_PRIVATE, &m);
    if (m) {
        int r = mprotect(m, len, PROT_READ | PROT_EXEC);
        check("mprotect() up to PROT_EXEC is refused with EACCES", r == -1 && errno == EACCES);
        munmap(m, len);
    }

    dirty_cow_race(1.0);

    check("the file is byte-for-byte unchanged", file_intact());
    return failures ? 1 : 0;
}
