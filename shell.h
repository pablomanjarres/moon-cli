#ifndef SHELL_H
#define SHELL_H

/**
 * ====================================================================================
 * shell.h — THE CONTRACT
 * ====================================================================================
 * Everything the files of this project share:
 *   - the Command struct (what a command is)
 *   - the command and category tables (they live in registry.c)
 *   - the prototype of every command (implemented in the cat_*.c files)
 *
 * File map:
 *   main.c          the main loop (read -> evaluate -> print)
 *   parser.c        splits a line into arguments, honoring "double quotes"
 *   registry.c      command table + help + suggestions
 *   ui.c / ui.h     everything visual (colors, boxes, syscall tracing)
 *   cat_data.c      file commands      (open, read, write, stat...)
 *   cat_memory.c    memory commands    (sbrk, mmap...)
 *   cat_process.c   process commands   (fork, exec, kill, getrusage...)
 *   cat_util.c      small utilities    (greet, time, date...)
 *   cat_editor.c    the text editor    (open, read, write, lseek, ftruncate)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

/* ------------------------------------------------------------------------------------
 * Every command has this signature: it takes argc/argv like a normal program and
 * returns 0 on success, non-zero on failure (the POSIX convention).
 * ------------------------------------------------------------------------------------ */
typedef int (*CommandFn)(int argc, char **argv);

/**
 * A shell command plus its teaching metadata.
 *
 * If `handler` is NULL the command counts as PENDING: it still shows up in the help
 * marked with ○ and, if you type it, the shell tells you exactly which file to
 * implement it in. That way the table doubles as a to-do list.
 */
typedef struct {
    const char *name;        /* what the user types, e.g. "d_create"                 */
    const char *category;    /* "data" | "memory" | "process" | "util"               */
    const char *usage;       /* syntax, e.g. d_create <file> "<text>"                */
    const char *description; /* what it does, in one line                            */
    const char *syscalls;    /* syscalls involved, e.g. "open(2) · write(2)"         */
    const char *file;        /* file where it belongs (a hint for you)               */
    CommandFn   handler;     /* the function that runs it, or NULL if pending        */
} Command;

/** A category. Only used to group things nicely in the help. */
typedef struct {
    const char *name;   /* "data"                       */
    const char *title;  /* "Files and data"             */
    const char *hint;   /* summary of the group syscalls */
} Category;

/* Global tables (defined in registry.c) */
extern const Command  commands[];
extern const int      num_commands;
extern const Category categories[];
extern const int      num_categories;

/* ------------------------------------------------------------------------------------
 * registry.c
 * ------------------------------------------------------------------------------------ */
const Command *registry_find(const char *name);       /* exact lookup, NULL if missing  */
const char    *registry_suggest(const char *name);    /* "did you mean ...?"            */
void           registry_help(const char *arg);        /* help | help <cat> | help <cmd> */
void           registry_usage(const char *name);      /* "wrong usage" + the syntax     */

/* ------------------------------------------------------------------------------------
 * parser.c
 * ------------------------------------------------------------------------------------ */
int parse_line(char *line, char **argv, int max_args);

/* ====================================================================================
 * COMMAND PROTOTYPES
 * ====================================================================================
 * Every implemented command needs its prototype here. The commented-out ones are the
 * missing ones: uncomment each as you write it.
 */

/* --- data (cat_data.c) --- */
int cmd_d_create(int argc, char **argv);   /* open(2), write(2), close(2)       DONE */
/* int cmd_d_read (int argc, char **argv); */  /* open, read, close                  */
/* int cmd_d_info (int argc, char **argv); */  /* stat                               */
/* int cmd_d_copy (int argc, char **argv); */  /* open, read, write, close           */

/* --- memory (cat_memory.c) --- */
/* int cmd_m_sbrk(int argc, char **argv); */   /* sbrk / brk                         */
/* int cmd_m_mmap(int argc, char **argv); */   /* mmap, munmap                       */
/* int cmd_m_info(int argc, char **argv); */   /* /proc/self/status                  */

/* --- process (cat_process.c) --- */
int cmd_p_fork(int argc, char **argv);     /* fork, getpid, getppid, waitpid    DONE */
/* int cmd_p_exec   (int argc, char **argv); */ /* fork, execvp, waitpid             */
/* int cmd_p_kill   (int argc, char **argv); */ /* kill                              */
/* int cmd_p_monitor(int argc, char **argv); */ /* getrusage                         */

/* --- editor (cat_editor.c) --- */
int cmd_edit(int argc, char **argv);       /* open, read, write, lseek, ftruncate, close */

/* --- util (cat_util.c) --- */
int cmd_color(int argc, char **argv);          /* write(2), through printf     DONE */
/* int cmd_greet(int argc, char **argv); */    /* getuid, getpwuid                   */
/* int cmd_time (int argc, char **argv); */    /* time                               */
/* int cmd_date (int argc, char **argv); */    /* time                               */

#endif /* SHELL_H */
