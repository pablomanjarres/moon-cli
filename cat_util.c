/**
 * ====================================================================================
 * cat_util.c — CATEGORY "util": short, handy system commands
 * ====================================================================================
 * Syscalls in this family: getuid(2), time(2).
 *
 * THIS FILE IS EMPTY ON PURPOSE. These are the easiest commands in the shell, so
 * start here if you want to get the rhythm before touching memory or processes.
 *
 * PENDING:
 *   greet   getuid() + getpwuid() to find out who you are
 *   time    time() + localtime(), formatted as HH:MM:SS
 *   date    time() + localtime(), formatted as YYYY-MM-DD
 *
 * The recipe:
 *   1. write the function here
 *   2. uncomment its prototype in shell.h
 *   3. in registry.c, swap that row's NULL for the function name
 */

#include "shell.h"

/* Uncomment what you need once you start:
 *
 * #include <unistd.h>   // getuid
 * #include <pwd.h>      // getpwuid
 * #include <time.h>     // time, localtime, strftime
 */

/**
 * ------------------------------------------------------------------------------------
 * TEMPLATE: time
 * ------------------------------------------------------------------------------------
 * time() returns the number of seconds since January 1st 1970 (the "Unix epoch").
 * localtime() turns that huge number into day, month, hour and minute according to
 * the system's timezone.
 *
 * int cmd_time(int argc, char **argv)
 * {
 *     (void)argc; (void)argv;
 *
 *     ui_trace_begin("time");
 *
 *     ui_syscall("time(NULL)");
 *     time_t now = time(NULL);
 *     if (now == (time_t)-1) { ui_ret_err(); ui_trace_fail("time failed."); return 1; }
 *     ui_ret_int((long)now);
 *
 *     char buf[64];
 *     struct tm *t = localtime(&now);
 *     strftime(buf, sizeof(buf), "%H:%M:%S", t);
 *
 *     ui_trace_ok("It is %s.", buf);
 *     return 0;
 * }
 */
