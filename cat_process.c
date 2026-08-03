/**
 * ====================================================================================
 * cat_process.c — CATEGORY "process": processes, signals and resources
 * ====================================================================================
 * Syscalls in this family: fork(2), execvp(3), waitpid(2), kill(2), getrusage(2).
 *
 * IMPLEMENTED HERE:
 *   p_fork     <- full example, use it as your template
 *
 * PENDING (your turn):
 *   p_exec     fork + execvp + waitpid   (this is, literally, what bash does)
 *   p_kill     kill
 *   p_monitor  getrusage
 */

#include "shell.h"

#include <unistd.h>     /* fork, getpid, getppid, sleep, _exit */
#include <sys/wait.h>   /* waitpid, WIFEXITED, WEXITSTATUS     */
#include <sys/types.h>  /* pid_t                               */
#include <errno.h>

/**
 * ------------------------------------------------------------------------------------
 * p_fork
 * ------------------------------------------------------------------------------------
 * The weirdest syscall to learn: fork() is called ONCE and returns TWICE.
 *
 *   - The kernel duplicates the entire process (memory, descriptors, where it is in
 *     the code).
 *   - In the parent, fork() returns the child's PID.
 *   - In the child, fork() returns 0.
 *   - If it fails it returns -1 and there is no child.
 *
 * From that line on there are two programs running the same code; the `if` below is
 * the only thing telling them apart.
 *
 * A detail that is critical and very easy to forget:
 *   printf() does not write to the screen, it writes into a BUFFER in the process's
 *   memory. Call fork() with a half-full buffer and the child inherits a copy of it,
 *   so that text ends up printed TWICE. That is why we call fflush(stdout) right
 *   before fork(). Try removing it and watch what happens.
 */
int cmd_p_fork(int argc, char **argv)
{
    (void)argc;   /* this command takes no arguments */
    (void)argv;

    ui_trace_begin("p_fork");

    /* --- 1. who am I? -------------------------------------------------------------- */
    ui_syscall("getpid()");
    pid_t me = getpid();
    ui_ret_int(me);

    /* --- 2. fork ------------------------------------------------------------------- */
    ui_syscall("fork()");
    fflush(stdout);              /* flush the buffer BEFORE duplicating the process */

    pid_t child = fork();

    if (child == -1) {
        ui_ret_err();
        ui_trace_fail("The kernel could not create the child process.");
        return 1;
    }

    /* ================= from here on, TWO processes are running ==================== */

    if (child == 0) {
        /* ---------- The CHILD's path ---------- */
        sleep(1);                /* syscall nanosleep(2): so you can see it is alive */
        ui_trace_note("child   pid %d   ppid %d   ·   now calling _exit(42)",
                      getpid(), getppid());
        fflush(stdout);
        _exit(42);               /* _exit and not exit(): the child must not flush
                                  * again the buffers it inherited from the parent */
    }

    /* ---------- The PARENT's path ---------- */
    ui_ret_int(child);           /* in the parent, fork() returned the child's PID */
    ui_trace_note("parent  pid %d   ·   the child is a copy with a different PID", me);
    ui_trace_note("the parent blocks in waitpid() until the child is done…");
    fflush(stdout);

    /* --- 3. waitpid ---------------------------------------------------------------- *
     * While the parent sits blocked here, the child prints its line. That is why the
     * call is traced afterwards: this way the two processes never write over each
     * other. If the parent did NOT wait, the child would become a zombie — finished,
     * but with its exit status never collected. */
    int status = 0;
    pid_t finished = waitpid(child, &status, 0);

    ui_syscall("waitpid(%d, &status, 0)", child);
    if (finished == -1) {
        ui_ret_err();
        ui_trace_fail("Could not wait for child %d.", child);
        return 1;
    }
    ui_ret_int(finished);

    /* `status` is not the exit code: it is a packed integer. These macros unpack it. */
    if (WIFEXITED(status)) {
        ui_trace_ok("Child %d exited on its own with code %d.",
                    finished, WEXITSTATUS(status));
        return 0;
    }
    if (WIFSIGNALED(status)) {
        ui_trace_fail("Child %d was killed by signal %d.", finished, WTERMSIG(status));
        return 1;
    }

    ui_trace_ok("Child %d changed state.", finished);
    return 0;
}

/**
 * ====================================================================================
 * TEMPLATE — the natural next command is p_exec
 * ====================================================================================
 * p_exec is the heart of every real shell: fork() to get a spare process, execvp() to
 * turn that process into another program, waitpid() to collect the result. Watch out:
 * if execvp() succeeds it NEVER returns (the old program stops existing); the line
 * after it only runs when it failed.
 *
 * int cmd_p_exec(int argc, char **argv)
 * {
 *     if (argc < 2) { registry_usage("p_exec"); return 1; }
 *
 *     ui_trace_begin("p_exec");
 *     ui_syscall("fork()");
 *     fflush(stdout);
 *     pid_t child = fork();
 *     if (child == -1) { ui_ret_err(); ui_trace_fail("Out of resources."); return 1; }
 *
 *     if (child == 0) {
 *         execvp(argv[1], &argv[1]);
 *         // getting here means execvp failed
 *         ui_ret_err();
 *         _exit(127);
 *     }
 *
 *     ui_ret_int(child);
 *     int status = 0;
 *     waitpid(child, &status, 0);
 *     ui_syscall("waitpid(%d, &status, 0)", child);
 *     ui_ret_int(child);
 *     ui_trace_ok("'%s' exited with code %d.", argv[1], WEXITSTATUS(status));
 *     return 0;
 * }
 */
