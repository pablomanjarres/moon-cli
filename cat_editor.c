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

static int ed_open(const char *path)
{
    if (!path || !*path) {
        printf("  usage: o <file>\n");
        return 1;
    }

    ed_close();

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    ed_fd = fd;
    snprintf(ed_path, sizeof ed_path, "%s", path);
    printf("  %s%s%s  fd %d\n", C_BRAND, path, C_OFF, fd);
    return 0;
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
        if (strcmp(cmd, "o") == 0) { ed_open(arg); continue; }

        if (ed_fd == -1) {
            printf("  %sno file open%s  use: o <file>\n", C_WARN, C_OFF);
            continue;
        }

        printf("  %s?%s o p a d i s q\n", C_MUTED, C_OFF);
    }

    ed_close();
    return 0;
}
