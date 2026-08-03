/**
 * ====================================================================================
 * cat_data.c — CATEGORY "data": files and the filesystem
 * ====================================================================================
 * Syscalls in this family: open(2), read(2), write(2), close(2), stat(2).
 *
 * IMPLEMENTED HERE:
 *   d_create   <- full example, use it as your template
 *
 * PENDING (your turn):
 *   d_read     open + read + close
 *   d_info     stat
 *   d_copy     open + read + write + close (in a loop)
 */

#include "shell.h"

#include <fcntl.h>      /* open, O_WRONLY, O_CREAT, O_TRUNC */
#include <unistd.h>     /* write, close, read               */
#include <errno.h>      /* errno                            */

/**
 * ------------------------------------------------------------------------------------
 * d_create <file> "<text>"
 * ------------------------------------------------------------------------------------
 * Creates (or truncates) a file and writes text into it using nothing but syscalls.
 *
 * The three calls, in order:
 *
 *   open(path, flags, mode)
 *       Asks the kernel for a way into the file. It returns a *file descriptor*: a
 *       plain integer that indexes this process's table of open files. Numbers 0, 1
 *       and 2 are already taken (stdin, stdout, stderr), so the first one you get is
 *       usually 3.
 *         O_WRONLY  write only
 *         O_CREAT   create it if it does not exist
 *         O_TRUNC   if it did exist, cut it back to 0 bytes
 *         0644      octal permissions (-rw-r--r--) applied only if it gets created
 *
 *   write(fd, buffer, n)
 *       Copies n bytes from the process's memory into the file. It returns how many
 *       bytes it actually wrote, which can be fewer than you asked for (rare on
 *       regular files, completely normal on pipes and sockets).
 *
 *   close(fd)
 *       Frees the slot in the descriptor table. Skip it and you leak descriptors
 *       until the process runs out of them.
 */
int cmd_d_create(int argc, char **argv)
{
    if (argc != 3) {
        registry_usage("d_create");
        return 1;
    }

    const char *path = argv[1];
    const char *text = argv[2];
    size_t len = strlen(text);

    ui_trace_begin("d_create");

    /* --- 1. open ------------------------------------------------------------------ */
    ui_syscall("open(\"%s\", O_WRONLY|O_CREAT|O_TRUNC, 0644)", path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        ui_ret_err();                       /* prints -1 plus the errno name */
        ui_trace_fail("Could not create '%s'.", path);
        return 1;
    }
    ui_ret_int(fd);
    ui_trace_note("fd %d = index into this process's table of open files", fd);

    /* --- 2. write ----------------------------------------------------------------- */
    ui_syscall("write(%d, \"%s\", %zu)", fd, text, len);
    ssize_t written = write(fd, text, len);
    if (written == -1) {
        ui_ret_err();
        close(fd);                          /* failure or not, close the descriptor */
        ui_trace_fail("Could not write to '%s'.", path);
        return 1;
    }
    ui_ret_int(written);

    /* --- 3. close ----------------------------------------------------------------- */
    ui_syscall("close(%d)", fd);
    if (close(fd) == -1) {
        ui_ret_err();
        ui_trace_fail("The data was written but the file did not close cleanly.");
        return 1;
    }
    ui_ret_int(0);

    ui_trace_ok("'%s' created with %zd bytes.", path, written);
    return 0;
}

/**
 * ====================================================================================
 * TEMPLATE — copy this for the next command
 * ====================================================================================
 * The steps are always the same:
 *   1. write the function down here
 *   2. uncomment its prototype in shell.h
 *   3. in registry.c, swap that row's NULL for the function name
 *
 * int cmd_d_read(int argc, char **argv)
 * {
 *     if (argc != 2) { registry_usage("d_read"); return 1; }
 *     const char *path = argv[1];
 *     char buffer[1024];
 *
 *     ui_trace_begin("d_read");
 *
 *     ui_syscall("open(\"%s\", O_RDONLY)", path);
 *     int fd = open(path, O_RDONLY);
 *     if (fd == -1) { ui_ret_err(); ui_trace_fail("No such file '%s'.", path); return 1; }
 *     ui_ret_int(fd);
 *
 *     ui_syscall("read(%d, buffer, %zu)", fd, sizeof(buffer) - 1);
 *     ssize_t got = read(fd, buffer, sizeof(buffer) - 1);
 *     if (got == -1) { ui_ret_err(); close(fd); ui_trace_fail("Read failed."); return 1; }
 *     ui_ret_int(got);
 *     buffer[got] = '\0';
 *
 *     ui_syscall("close(%d)", fd);
 *     ui_ret_int(close(fd));
 *
 *     ui_trace_ok("%zd bytes read.", got);
 *
 *     ui_box_top("contents");
 *     ui_box_line("%s", buffer);
 *     ui_box_bottom();
 *     return 0;
 * }
 */
