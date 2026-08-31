#include "shell.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static int  ed_fd = -1;
static char ed_path[512];

static void ed_close(void)
{
    if (ed_fd == -1) return;
    if (close(ed_fd) == -1) perror("close");
    ed_fd = -1;
    ed_path[0] = '\0';
}

int cmd_edit(int argc, char **argv)
{
    char line[2048];

    (void)argc;
    (void)argv;

    for (;;) {
        printf("  %sed%s %s ", C_BRAND, C_OFF, ICON_PROMPT);
        fflush(stdout);

        if (!fgets(line, sizeof line, stdin)) break;

        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *cmd = line;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (!*cmd) continue;

        char *arg = cmd;
        while (*arg && *arg != ' ' && *arg != '\t') arg++;
        if (*arg) {
            *arg++ = '\0';
            while (*arg == ' ' || *arg == '\t') arg++;
        }

        if (strcmp(cmd, "q") == 0) break;

        printf("  %s?%s o p a d i s q\n", C_MUTED, C_OFF);
    }

    ed_close();
    return 0;
}
