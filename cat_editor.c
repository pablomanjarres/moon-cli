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

static char *ed_slurp(size_t *len)
{
    off_t end = lseek(ed_fd, 0, SEEK_END);
    if (end == -1) { perror("lseek"); return NULL; }
    if (lseek(ed_fd, 0, SEEK_SET) == -1) { perror("lseek"); return NULL; }

    char *buf = malloc((size_t)end + 1);
    if (!buf) { perror("malloc"); return NULL; }

    size_t got = 0;
    while (got < (size_t)end) {
        ssize_t n = read(ed_fd, buf + got, (size_t)end - got);
        if (n == -1) { perror("read"); free(buf); return NULL; }
        if (n == 0) break;
        got += (size_t)n;
    }

    buf[got] = '\0';
    *len = got;
    return buf;
}

static int ed_out(const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(1, buf + off, len - off);
        if (n == -1) { perror("write"); return -1; }
        off += (size_t)n;
    }
    return 0;
}

static char *ed_line(char *buf, size_t len, int n, size_t *out_len)
{
    if (n < 1) return NULL;

    size_t i = 0;
    int cur = 1;
    while (i < len && cur < n) {
        if (buf[i] == '\n') cur++;
        i++;
    }
    if (cur != n || i >= len) return NULL;

    size_t j = i;
    while (j < len && buf[j] != '\n') j++;

    *out_len = j - i;
    return buf + i;
}

static int ed_print(const char *arg)
{
    size_t len;
    char *buf = ed_slurp(&len);
    if (!buf) return 1;

    int rc = 0;
    if (!arg || !*arg) {
        if (len) {
            rc = ed_out(buf, len) == -1;
            if (!rc && buf[len - 1] != '\n') rc = ed_out("\n", 1) == -1;
        }
    } else {
        size_t ll;
        char *l = ed_line(buf, len, atoi(arg), &ll);
        if (!l) {
            printf("  %sno such line%s\n", C_WARN, C_OFF);
            rc = 1;
        } else {
            rc = ed_out(l, ll) == -1 || ed_out("\n", 1) == -1;
        }
    }

    free(buf);
    return rc;
}

static int ed_write(const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(ed_fd, buf + off, len - off);
        if (n == -1) { perror("write"); return -1; }
        off += (size_t)n;
    }
    return 0;
}

static int ed_append(const char *text)
{
    if (!text) text = "";

    off_t end = lseek(ed_fd, 0, SEEK_END);
    if (end == -1) { perror("lseek"); return 1; }

    if (end > 0) {
        char last;
        if (lseek(ed_fd, end - 1, SEEK_SET) == -1) { perror("lseek"); return 1; }
        if (read(ed_fd, &last, 1) == -1) { perror("read"); return 1; }
        if (last != '\n' && ed_write("\n", 1) == -1) return 1;
    }

    if (ed_write(text, strlen(text)) == -1) return 1;
    if (ed_write("\n", 1) == -1) return 1;
    return 0;
}

static int ed_save(const char *buf, size_t len)
{
    if (lseek(ed_fd, 0, SEEK_SET) == -1) { perror("lseek"); return -1; }
    if (ed_write(buf, len) == -1) return -1;
    if (ftruncate(ed_fd, (off_t)len) == -1) { perror("ftruncate"); return -1; }
    return 0;
}

static int ed_delete(const char *arg)
{
    if (!arg || !*arg) {
        printf("  usage: d <n>\n");
        return 1;
    }

    size_t len;
    char *buf = ed_slurp(&len);
    if (!buf) return 1;

    size_t ll;
    char *l = ed_line(buf, len, atoi(arg), &ll);
    if (!l) {
        printf("  %sno such line%s\n", C_WARN, C_OFF);
        free(buf);
        return 1;
    }

    size_t cut = ll + (l + ll < buf + len ? 1 : 0);
    size_t head = (size_t)(l - buf);
    memmove(l, l + cut, len - head - cut);

    int rc = ed_save(buf, len - cut) == -1;
    free(buf);
    return rc;
}

static int ed_insert(char *arg)
{
    if (!arg || !*arg) {
        printf("  usage: i <n> <text>\n");
        return 1;
    }

    char *text = arg;
    while (*text && *text != ' ' && *text != '\t') text++;
    if (*text) {
        *text++ = '\0';
        while (*text == ' ' || *text == '\t') text++;
    }

    int n = atoi(arg);
    if (n < 1) {
        printf("  usage: i <n> <text>\n");
        return 1;
    }

    size_t len;
    char *buf = ed_slurp(&len);
    if (!buf) return 1;

    size_t ll;
    char *l = ed_line(buf, len, n, &ll);
    size_t at = l ? (size_t)(l - buf) : len;

    size_t tl = strlen(text);
    char *nb = malloc(len + tl + 2);
    if (!nb) { perror("malloc"); free(buf); return 1; }

    size_t p = 0;
    memcpy(nb, buf, at);
    p = at;
    if (p && nb[p - 1] != '\n') nb[p++] = '\n';
    memcpy(nb + p, text, tl);
    p += tl;
    nb[p++] = '\n';
    memcpy(nb + p, buf + at, len - at);
    p += len - at;

    int rc = ed_save(nb, p) == -1;
    free(buf);
    free(nb);
    return rc;
}

static int ed_search(const char *word)
{
    if (!word || !*word) {
        printf("  usage: s <word>\n");
        return 1;
    }

    size_t len;
    char *buf = ed_slurp(&len);
    if (!buf) return 1;

    int hits = 0;
    size_t i = 0;
    int n = 1;

    while (i < len) {
        size_t j = i;
        while (j < len && buf[j] != '\n') j++;

        char save = buf[j];
        buf[j] = '\0';
        if (strstr(buf + i, word)) {
            printf("  %s%d%s  %s\n", C_ACCENT, n, C_OFF, buf + i);
            hits++;
        }
        buf[j] = save;

        i = j + 1;
        n++;
    }

    if (!hits) printf("  %snot found%s\n", C_MUTED, C_OFF);

    free(buf);
    return hits ? 0 : 1;
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

        if (strcmp(cmd, "p") == 0) ed_print(arg);
        else if (strcmp(cmd, "a") == 0) ed_append(arg);
        else if (strcmp(cmd, "d") == 0) ed_delete(arg);
        else if (strcmp(cmd, "i") == 0) ed_insert(arg);
        else if (strcmp(cmd, "s") == 0) ed_search(arg);
        else printf("  %s?%s o p a d i s q\n", C_MUTED, C_OFF);
    }

    ed_close();
    return 0;
}
