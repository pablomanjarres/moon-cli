/**
 * ====================================================================================
 * cat_memory.c — CATEGORY "memory": the process address space
 * ====================================================================================
 * Syscalls in this family: sbrk(2)/brk(2), mmap(2), mprotect(2), munmap(2), plus
 * reading /proc.
 *
 * IMPLEMENTED HERE:
 *   m_fmap <file> <mode>   map a file and touch it; shows what kernel/mmap_guard.ko
 *                          allows and what it refuses (Parcial 3)
 *
 * PENDING:
 *   m_sbrk <bytes>   move the "program break" (the top of the heap) and show the
 *                    address before and after. malloc() uses this underneath.
 *   m_mmap <bytes>   ask the kernel for a fresh region of memory, write a pattern
 *                    into it, then hand it back with munmap().
 *   m_info           read /proc/self/status and show VmRSS, VmSize, and friends.
 *
 * The recipe for each one (always the same):
 *   1. write the function here
 *   2. uncomment its prototype in shell.h
 *   3. in registry.c, swap that row's NULL for the function name
 */

#include "shell.h"

#include <fcntl.h>      /* open                                  */
#include <unistd.h>     /* close                                 */
#include <sys/mman.h>   /* mmap, mprotect, msync, munmap         */
#include <sys/stat.h>   /* fstat                                 */

/**
 * ------------------------------------------------------------------------------------
 * m_fmap <file> <r|w|p|x|u>
 * ------------------------------------------------------------------------------------
 * Maps a file the way a program would, then touches the first page through a pointer.
 * That first touch is a page fault: the kernel fills the page from the page cache and
 * no other syscall happens. That is why the guard has to act at mmap() time.
 *
 *   r   PROT_READ,             MAP_SHARED    read the file in place
 *   w   PROT_READ|PROT_WRITE,  MAP_SHARED    write "MOON" into the file itself
 *   p   PROT_READ|PROT_WRITE,  MAP_PRIVATE   write a private copy (copy-on-write)
 *   x   PROT_READ|PROT_EXEC,   MAP_PRIVATE   map the file as code
 *   u   PROT_READ, MAP_SHARED, then mprotect() up to PROT_WRITE
 *
 * With kernel/mmap_guard.ko guarding the file, w, x and u come back -1 EACCES.
 */
static int fmap_flags(char mode, int *oflag, int *prot, int *flags)
{
    switch (mode) {
    case 'r': *oflag = O_RDONLY; *prot = PROT_READ;              *flags = MAP_SHARED;  return 0;
    case 'w': *oflag = O_RDWR;   *prot = PROT_READ | PROT_WRITE; *flags = MAP_SHARED;  return 0;
    case 'p': *oflag = O_RDONLY; *prot = PROT_READ | PROT_WRITE; *flags = MAP_PRIVATE; return 0;
    case 'x': *oflag = O_RDONLY; *prot = PROT_READ | PROT_EXEC;  *flags = MAP_PRIVATE; return 0;
    case 'u': *oflag = O_RDWR;   *prot = PROT_READ;              *flags = MAP_SHARED;  return 0;
    }
    return -1;
}

static const char *prot_name(int prot)
{
    if (prot & PROT_EXEC)  return "PROT_READ|PROT_EXEC";
    if (prot & PROT_WRITE) return "PROT_READ|PROT_WRITE";
    return "PROT_READ";
}

int cmd_m_fmap(int argc, char **argv)
{
    int oflag, prot, flags;

    if (argc != 3 || strlen(argv[2]) != 1 || fmap_flags(argv[2][0], &oflag, &prot, &flags)) {
        registry_usage("m_fmap");
        return 1;
    }
    const char *path = argv[1];
    char mode = argv[2][0];

    ui_trace_begin("m_fmap");

    /* --- 1. open + fstat: the mapping length is the file size ----------------------- */
    ui_syscall("open(\"%s\", %s)", path, oflag == O_RDWR ? "O_RDWR" : "O_RDONLY");
    int fd = open(path, oflag);
    if (fd == -1) { ui_ret_err(); ui_trace_fail("Could not open '%s'.", path); return 1; }
    ui_ret_int(fd);

    struct stat st;
    ui_syscall("fstat(%d, &st)", fd);
    if (fstat(fd, &st) == -1) {
        ui_ret_err(); close(fd); ui_trace_fail("fstat failed."); return 1;
    }
    ui_ret_int(0);
    if (st.st_size == 0) {
        close(fd); ui_trace_fail("'%s' is empty: there is nothing to map.", path); return 1;
    }
    size_t len = (size_t)st.st_size;

    /* --- 2. mmap: the only moment the kernel can say no ------------------------------ */
    ui_syscall("mmap(NULL, %zu, %s, %s, %d, 0)", len, prot_name(prot),
               flags == MAP_SHARED ? "MAP_SHARED" : "MAP_PRIVATE", fd);
    char *map = mmap(NULL, len, prot, flags, fd, 0);
    if (map == MAP_FAILED) {
        ui_ret_err(); close(fd);
        ui_trace_fail("The kernel refused the mapping.");
        return 1;
    }
    ui_ret_ptr(map);

    /* --- 3. mode u: widen a read-only shared mapping afterwards ---------------------- */
    if (mode == 'u') {
        ui_syscall("mprotect(%p, %zu, PROT_READ|PROT_WRITE)", (void *)map, len);
        if (mprotect(map, len, PROT_READ | PROT_WRITE) == -1) {
            ui_ret_err(); munmap(map, len); close(fd);
            ui_trace_fail("The kernel refused to make the mapping writable.");
            return 1;
        }
        ui_ret_int(0);
    }

    /* --- 4. touch the page: a page fault, not a syscall ------------------------------ */
    ui_trace_note("first access to %p -> page fault, no syscall", (void *)map);
    char first[17];
    size_t show = len < 16 ? len : 16;
    memcpy(first, map, show);
    first[show] = '\0';
    for (size_t i = 0; i < show; i++)
        if (first[i] < 32 || first[i] > 126) first[i] = '.';

    int wrote = (mode == 'w' || mode == 'p' || mode == 'u');
    if (wrote) {
        size_t n = len < 4 ? len : 4;
        memcpy(map, "MOON", n);
        ui_trace_note("wrote \"MOON\" through the pointer (%s)",
                      flags == MAP_SHARED ? "reaches the file" : "private copy only");
    }

    if (wrote && flags == MAP_SHARED) {
        ui_syscall("msync(%p, %zu, MS_SYNC)", (void *)map, len);
        if (msync(map, len, MS_SYNC) == -1) ui_ret_err(); else ui_ret_int(0);
    }

    /* --- 5. release ------------------------------------------------------------------ */
    ui_syscall("munmap(%p, %zu)", (void *)map, len);
    ui_ret_int(munmap(map, len));
    ui_syscall("close(%d)", fd);
    ui_ret_int(close(fd));

    ui_trace_ok("Mapped %zu bytes of '%s'.", len, path);

    ui_box_top("mapping");
    ui_box_kv("First bytes", "%s", first);
    ui_box_kv("Protection",  "%s", prot_name(mode == 'u' ? PROT_WRITE : prot));
    ui_box_kv("Sharing",     "%s", flags == MAP_SHARED ? "MAP_SHARED" : "MAP_PRIVATE");
    ui_box_bottom();
    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * TEMPLATE: m_sbrk <increment_bytes>
 * ------------------------------------------------------------------------------------
 * sbrk(0) moves nothing: it just reports where the program break currently sits. Call
 * it before and after and you can see exactly how much the heap grew.
 *
 * int cmd_m_sbrk(int argc, char **argv)
 * {
 *     if (argc != 2) { registry_usage("m_sbrk"); return 1; }
 *     long inc = atol(argv[1]);
 *
 *     ui_trace_begin("m_sbrk");
 *
 *     ui_syscall("sbrk(0)");
 *     void *before = sbrk(0);
 *     if (before == (void *)-1) { ui_ret_err(); ui_trace_fail("sbrk failed."); return 1; }
 *     ui_ret_ptr(before);
 *
 *     ui_syscall("sbrk(%ld)", inc);
 *     void *previous = sbrk(inc);
 *     if (previous == (void *)-1) { ui_ret_err(); ui_trace_fail("sbrk failed."); return 1; }
 *     ui_ret_ptr(previous);
 *
 *     ui_syscall("sbrk(0)");
 *     void *after = sbrk(0);
 *     ui_ret_ptr(after);
 *
 *     ui_trace_ok("The heap moved %ld bytes.", (long)((char *)after - (char *)before));
 *
 *     ui_box_top("heap");
 *     ui_box_kv("Break before", "%p", before);
 *     ui_box_kv("Break after",  "%p", after);
 *     ui_box_kv("Difference",   "%ld bytes", (long)((char *)after - (char *)before));
 *     ui_box_bottom();
 *     return 0;
 * }
 */
