/**
 * ====================================================================================
 * registry.c — THE COMMAND TABLE AND THE HELP
 * ====================================================================================
 * This file is the index of the shell. Adding a new command is 3 steps:
 *
 *   1. Write the function in the matching cat_*.c:
 *          int cmd_d_read(int argc, char **argv) { ... }
 *   2. Uncomment (or add) its prototype in shell.h.
 *   3. Down below, swap that row's NULL for the function name.
 *
 * That is all. The main loop, the help and the suggestions pick it up on their own.
 */

#include "shell.h"

/* ====================================================================================
 * 1. CATEGORIES
 * ==================================================================================== */
const Category categories[] = {
    { "data",    "Files and data",     "open · read · write · stat"     },
    { "memory",  "Process memory",     "sbrk · mmap · /proc"            },
    { "process", "Processes & signals", "fork · exec · kill · getrusage" },
    { "util",    "Utilities",          "getuid · time"                  },
};
const int num_categories = (int)(sizeof(categories) / sizeof(categories[0]));

/* ====================================================================================
 * 2. COMMAND TABLE
 * ====================================================================================
 * handler == NULL  =>  the command is PENDING (shown with ○ in the help).
 * Write the metadata even before implementing it: the help then reminds you what is
 * missing, which file it goes in and which syscalls it needs.
 */
const Command commands[] = {
    /* --- data -------------------------------------------------------------------- */
    { "d_create", "data",
      "d_create <file> \"<text>\"",
      "Create a file and write text into it.",
      "open(2) · write(2) · close(2)",
      "cat_data.c",
      cmd_d_create },                      /* <- IMPLEMENTED */

    { "d_read", "data",
      "d_read <file>",
      "Read a file and print its contents.",
      "open(2) · read(2) · close(2)",
      "cat_data.c",
      NULL },

    { "d_info", "data",
      "d_info <file>",
      "Show a file's metadata (inode, permissions, size).",
      "stat(2)",
      "cat_data.c",
      NULL },

    { "d_copy", "data",
      "d_copy <source> <destination>",
      "Copy a file block by block.",
      "open(2) · read(2) · write(2) · close(2)",
      "cat_data.c",
      NULL },

    /* --- memory ------------------------------------------------------------------ */
    { "m_sbrk", "memory",
      "m_sbrk <increment_bytes>",
      "Move the program break and watch the heap grow.",
      "sbrk(2) / brk(2)",
      "cat_memory.c",
      NULL },

    { "m_mmap", "memory",
      "m_mmap <size_bytes>",
      "Map anonymous memory, write a pattern, release it.",
      "mmap(2) · munmap(2)",
      "cat_memory.c",
      NULL },

    { "m_info", "memory",
      "m_info",
      "Show the shell's own memory usage.",
      "reads /proc/self/status",
      "cat_memory.c",
      NULL },

    /* --- process ----------------------------------------------------------------- */
    { "p_fork", "process",
      "p_fork",
      "Spawn a child process and wait for its exit code.",
      "fork(2) · getpid(2) · getppid(2) · waitpid(2)",
      "cat_process.c",
      cmd_p_fork },                        /* <- IMPLEMENTED */

    { "p_exec", "process",
      "p_exec <command> [args...]",
      "Run an external program inside a child process.",
      "fork(2) · execvp(3) · waitpid(2)",
      "cat_process.c",
      NULL },

    { "p_kill", "process",
      "p_kill <pid> <signal>",
      "Send a signal to a process.",
      "kill(2)",
      "cat_process.c",
      NULL },

    { "p_monitor", "process",
      "p_monitor",
      "Show the shell's CPU and memory usage.",
      "getrusage(2)",
      "cat_process.c",
      NULL },

    /* --- util -------------------------------------------------------------------- */
    { "color", "util",
      "color <text> [--<color>=<index>]",
      "Print text with one color per letter.",
      "write(2), through printf",
      "cat_util.c",
      cmd_color },                         /* <- IMPLEMENTED */

    { "greet", "util",
      "greet",
      "Greet the current user by reading their UID.",
      "getuid(2)",
      "cat_util.c",
      NULL },

    { "time", "util",
      "time",
      "Show the system time (HH:MM:SS).",
      "time(2)",
      "cat_util.c",
      NULL },

    { "date", "util",
      "date",
      "Show the system date (YYYY-MM-DD).",
      "time(2)",
      "cat_util.c",
      NULL },
};
const int num_commands = (int)(sizeof(commands) / sizeof(commands[0]));

/* ====================================================================================
 * 3. LOOKUP
 * ==================================================================================== */

const Command *registry_find(const char *name)
{
    for (int i = 0; i < num_commands; i++)
        if (strcmp(commands[i].name, name) == 0)
            return &commands[i];
    return NULL;
}

/**
 * Wrong-usage error, written once for the whole shell: the help text comes straight
 * from the table, so it can never go stale.
 */
void registry_usage(const char *name)
{
    const Command *c = registry_find(name);
    if (!c) { ui_fail("Wrong usage of '%s'.", name); return; }

    ui_fail("Wrong usage of %s.", c->name);
    ui_hint("%s%s%s", C_ACCENT, c->usage, C_OFF);
    ui_hint("%s%s%s", C_DIM, c->description, C_OFF);
}

/* Levenshtein distance: how many edits (insert/delete/replace one letter) separate
 * two words. This is what powers the "did you mean ...?" hint. */
#define MAXW 48

static int levenshtein(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la >= MAXW || lb >= MAXW) return 99;

    int prev[MAXW + 1], cur[MAXW + 1];
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;

    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del  = prev[j] + 1;
            int ins  = cur[j - 1] + 1;
            int sub  = prev[j - 1] + cost;
            int best = del < ins ? del : ins;
            cur[j] = best < sub ? best : sub;
        }
        for (size_t j = 0; j <= lb; j++) prev[j] = cur[j];
    }
    return prev[lb];
}

const char *registry_suggest(const char *name)
{
    const char *best = NULL;
    int best_d = 99;

    for (int i = 0; i < num_commands; i++) {
        int d = levenshtein(name, commands[i].name);
        if (d < best_d) { best_d = d; best = commands[i].name; }
    }
    /* Only suggest when it really is close (tolerance scales with the length). */
    size_t len = strlen(name);
    int tolerance = len <= 4 ? 1 : (len <= 8 ? 2 : 3);
    return (best_d <= tolerance) ? best : NULL;
}

/* ====================================================================================
 * 4. HELP
 * ==================================================================================== */

/* One command row:  ● d_create   Create a file...  */
static void command_row(const Command *c, int with_syscalls)
{
    int done = (c->handler != NULL);
    printf("    %s%s%s ", done ? C_BRAND : C_DIM,
           done ? ICON_BULLET : ICON_TODO, C_OFF);

    size_t w = ui_width(c->name);
    printf("%s%s%s", done ? C_BRAND : C_DIM, c->name, C_OFF);
    for (size_t i = 0, n = (w < 12) ? 12 - w : 1; i < n; i++) putchar(' ');

    printf("%s%s%s\n", done ? C_MUTED : C_DIM, c->description, C_OFF);

    if (with_syscalls)
        printf("      %s%s%s\n", C_DIM, c->syscalls, C_OFF);
}

/* Same geometry as command_row, but for the shell built-ins (help/clear/exit),
 * which are not in the table. */
static void builtin_row(const char *name, const char *desc)
{
    printf("    %s%s%s ", C_DIM, ICON_DOT, C_OFF);
    size_t w = ui_width(name);
    printf("%s%s%s", C_BRAND, name, C_OFF);
    for (size_t i = 0, n = (w < 12) ? 12 - w : 1; i < n; i++) putchar(' ');
    printf("%s%s%s\n", C_MUTED, desc, C_OFF);
}

/* Progress bar:  ███████░░░░░░░  2/14 */
static void progress(int done, int total)
{
    const int width = 18;
    int filled = total > 0 ? (done * width) / total : 0;

    printf("  %s", C_BRAND);
    for (int i = 0; i < filled; i++) printf("█");
    printf("%s", C_DIM);
    for (int i = filled; i < width; i++) printf("░");
    printf("%s  %s%d of %d commands implemented%s\n",
           C_OFF, C_MUTED, done, total, C_OFF);
}

static void help_overview(void)
{
    int done = 0;
    for (int i = 0; i < num_commands; i++)
        if (commands[i].handler) done++;

    ui_blank();
    ui_title_line();
    printf("  %s%s%s implemented   %s%s pending%s\n\n",
           C_BRAND, ICON_BULLET, C_OFF, C_DIM, ICON_TODO, C_OFF);

    for (int c = 0; c < num_categories; c++) {
        ui_section(categories[c].name);
        for (int i = 0; i < num_commands; i++)
            if (strcmp(commands[i].category, categories[c].name) == 0)
                command_row(&commands[i], 0);
        ui_blank();
    }

    ui_section("shell");
    builtin_row("help",  "This help. Also: help <category> | help <command>");
    builtin_row("clear", "Clear the screen");
    builtin_row("exit",  "Quit the shell (or Ctrl+D)");
    ui_blank();

    progress(done, num_commands);
    ui_blank();
    ui_divider();
    printf("\n  %s%s%s  every %spending%s command already has its slot — run %shelp <command>%s to see where\n\n",
           C_DIM, ICON_ARROW, C_OFF, C_DIM, C_OFF, C_BRAND, C_OFF);
}

static int help_category(const char *name)
{
    const Category *cat = NULL;
    for (int i = 0; i < num_categories; i++)
        if (strcmp(categories[i].name, name) == 0) cat = &categories[i];
    if (!cat) return 0;

    ui_blank();
    ui_section(cat->title);
    printf("    %s%s%s\n\n", C_DIM, cat->hint, C_OFF);
    for (int i = 0; i < num_commands; i++)
        if (strcmp(commands[i].category, cat->name) == 0)
            command_row(&commands[i], 1);
    ui_blank();
    return 1;
}

static int help_command(const char *name)
{
    const Command *c = registry_find(name);
    if (!c) return 0;

    ui_blank();
    ui_box_top(c->name);
    ui_box_kv("What it does", "%s", c->description);
    ui_box_kv("Usage",        "%s%s%s", C_ACCENT, c->usage, C_OFF);
    ui_box_kv("Syscalls",     "%s%s%s", C_ACCENT, c->syscalls, C_OFF);
    ui_box_kv("Category",     "%s", c->category);
    ui_box_sep();
    if (c->handler)
        ui_box_line("%s%s%s  implemented in %s%s%s",
                    C_BRAND, ICON_OK, C_OFF, C_BOLD, c->file, C_OFF);
    else
        ui_box_line("%s%s  pending: implement it in %s%s",
                    C_DIM, ICON_TODO, c->file, C_OFF);
    ui_box_bottom();
    ui_blank();
    return 1;
}

void registry_help(const char *arg)
{
    if (arg == NULL) { help_overview(); return; }
    if (help_category(arg)) return;
    if (help_command(arg))  return;

    ui_blank();
    ui_fail("I don't know '%s'.", arg);
    const char *s = registry_suggest(arg);
    if (s) ui_hint("Did you mean %s%s%s?", C_BRAND, s, C_OFF);
    else   ui_hint("Type %shelp%s to see everything.", C_BRAND, C_OFF);
    ui_blank();
}
