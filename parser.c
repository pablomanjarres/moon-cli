/**
 * ====================================================================================
 * parser.c — FROM A LINE OF TEXT TO argc/argv
 * ====================================================================================
 * The user types:
 *
 *     d_create hi.txt "Hello operating systems"
 *
 * and this turns it into exactly what any C program receives:
 *
 *     argc = 3
 *     argv = { "d_create", "hi.txt", "Hello operating systems", NULL }
 *
 * The classic trick: nothing gets copied. We write '\0' over the separators of the
 * line itself and argv[] points into that same buffer. That is why `line` is modified
 * in place and cannot be a string literal.
 */

#include "shell.h"

int parse_line(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;
    char *start = NULL;   /* start of the argument we are currently reading */
    int in_quote = 0;     /* are we inside "double quotes"?                 */

    while (*p) {
        if (!in_quote && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
            /* Whitespace outside quotes: closes the current argument, if any. */
            if (start) {
                *p = '\0';
                if (argc < max_args - 1) argv[argc++] = start;
                start = NULL;
            }
        } else if (*p == '"') {
            if (in_quote) {
                /* Closing quote. */
                *p = '\0';
                if (argc < max_args - 1) argv[argc++] = start;
                start = NULL;
                in_quote = 0;
            } else {
                /* Opening quote: the argument starts right after it. */
                in_quote = 1;
                start = p + 1;
            }
        } else if (start == NULL) {
            start = p;   /* first character of a new argument */
        }
        p++;
    }

    /* Did an argument run all the way to the end of the line? */
    if (start && argc < max_args - 1)
        argv[argc++] = start;

    argv[argc] = NULL;   /* POSIX convention: argv ends with NULL */
    return argc;
}
