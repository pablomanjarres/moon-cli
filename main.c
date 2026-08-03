/**
 * ====================================================================================
 * main.c — THE SHELL LOOP (REPL)
 * ====================================================================================
 * A shell is nothing more than this, repeated forever:
 *
 *      1. READ    read a line from the keyboard
 *      2. EVAL    split it into arguments and find which command it is
 *      3. PRINT   run it and print whatever comes out
 *      4. LOOP    go back to step 1
 *
 * Everything else (colors, help, the command table) lives in other files so this one
 * stays short enough to read in one sitting.
 */

#include "shell.h"

#include <unistd.h>
#include <signal.h>
#include <errno.h>

#define MAX_LINE 2048   /* longest line the user can type    */
#define MAX_ARGS 64     /* most arguments a command can take */

/* ====================================================================================
 * SIGNALS
 * ====================================================================================
 * Press Ctrl+C and the kernel sends SIGINT to the shell. By default that would kill
 * it. A real shell catches it, cancels the current line and redraws the prompt.
 * Because we do NOT set SA_RESTART, the read() that is waiting on the keyboard gets
 * interrupted and returns EINTR — that is how we tell "Ctrl+C" apart from "end of
 * input" further down.
 */
static volatile sig_atomic_t g_interrupted = 0;

static void on_sigint(int sig)
{
    (void)sig;
    g_interrupted = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;              /* no SA_RESTART: we want read() to be cut short */
    sigaction(SIGINT, &sa, NULL); /* syscall: rt_sigaction(2) */
}

/* ====================================================================================
 * FLAGS OF THE BINARY ITSELF (./moon --help)
 * ==================================================================================== */
static int handle_cli_flags(int argc, char **argv)
{
    if (argc < 2) return 0;

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        registry_help(NULL);
        return 1;
    }
    if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
        printf("moon %s\n", MOON_VERSION);
        return 1;
    }
    ui_blank();
    ui_warn("Unknown option: %s", argv[1]);
    ui_hint("Run %s./moon%s to open the shell, or %s./moon --help%s.",
            C_BRAND, C_OFF, C_BRAND, C_OFF);
    ui_blank();
    return 1;
}

/* ====================================================================================
 * RUN AN ALREADY-PARSED COMMAND
 * ====================================================================================
 * Returns the exit status: 0 when it went fine, anything else on failure. That number
 * shows up in the prompt (for example  ✖1 ), the same way bash shows $?.
 */
static int dispatch(int argc, char **argv)
{
    const Command *cmd = registry_find(argv[0]);

    /* Case 1: no command by that name exists. */
    if (cmd == NULL) {
        ui_blank();
        ui_fail("Command '%s' not found.", argv[0]);
        const char *guess = registry_suggest(argv[0]);
        if (guess)
            ui_hint("Did you mean %s%s%s?", C_BRAND, guess, C_OFF);
        else
            ui_hint("Type %shelp%s to list the commands.", C_BRAND, C_OFF);
        return 127;   /* 127 = "command not found", same as bash */
    }

    /* Case 2: it is in the table but you have not written it yet. */
    if (cmd->handler == NULL) {
        ui_blank();
        ui_warn("'%s' is not implemented yet.", cmd->name);
        ui_hint("Your turn: write %s%s%s in %s%s%s and hook it up in registry.c.",
                C_BRAND, cmd->name, C_OFF, C_BRAND, cmd->file, C_OFF);
        ui_hint("Syscalls you will need: %s%s%s", C_ACCENT, cmd->syscalls, C_OFF);
        return 1;
    }

    /* Case 3: run it. */
    ui_blank();
    return cmd->handler(argc, argv);
}

/* ====================================================================================
 * MAIN
 * ==================================================================================== */
int main(int argc, char **argv)
{
    char line[MAX_LINE];
    char *args[MAX_ARGS];
    int last_status = 0;

    ui_init();                          /* does this terminal do colors?  */
    if (handle_cli_flags(argc, argv))   /* ./moon --help | --version      */
        return 0;

    install_signal_handlers();
    ui_welcome();

    for (;;) {
        ui_prompt(last_status);

        /* --- 1. READ ------------------------------------------------------------- */
        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (g_interrupted) {            /* it was Ctrl+C, not end of input */
                g_interrupted = 0;
                clearerr(stdin);
                putchar('\n');
                last_status = 130;          /* 128 + SIGINT(2), like bash */
                continue;
            }
            break;                          /* a real Ctrl+D: we are done */
        }

        /* --- 2. EVAL ------------------------------------------------------------- */
        int n = parse_line(line, args, MAX_ARGS);
        if (n == 0)
            continue;                       /* empty line: ignore it */

        /* Built-ins: handled by the shell itself, no syscalls involved. */
        if (strcmp(args[0], "exit") == 0 || strcmp(args[0], "quit") == 0)
            break;

        if (strcmp(args[0], "clear") == 0) {
            printf("\033[H\033[2J");        /* ANSI: home the cursor + erase screen */
            fflush(stdout);
            last_status = 0;
            continue;
        }

        if (strcmp(args[0], "help") == 0) {
            registry_help(n > 1 ? args[1] : NULL);
            last_status = 0;
            continue;
        }

        /* --- 3. PRINT (inside the command) --------------------------------------- */
        last_status = dispatch(n, args);
    }

    ui_farewell();
    return 0;
}
