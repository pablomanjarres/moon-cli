/**
 * ====================================================================================
 * cat_memory.c — CATEGORY "memory": the process address space
 * ====================================================================================
 * Syscalls in this family: sbrk(2)/brk(2), mmap(2), munmap(2), plus reading /proc.
 *
 * THIS FILE IS EMPTY ON PURPOSE. Your turn.
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

/* Uncomment what you need once you start:
 *
 * #include <unistd.h>     // sbrk
 * #include <sys/mman.h>   // mmap, munmap
 * #include <errno.h>
 */

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
