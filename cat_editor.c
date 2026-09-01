#include "shell.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>

static int ed_fd = -1;

static void ed_close(void)
{
    if (ed_fd == -1) return;
    if (close(ed_fd) == -1) perror("close");
    ed_fd = -1;
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
        ssize_t got = read(ed_fd, &last, 1);
        if (got == -1) { perror("read"); return 1; }
        if (got != 1) {
            if (lseek(ed_fd, 0, SEEK_END) == -1) { perror("lseek"); return 1; }
        } else if (last != '\n' && ed_write("\n", 1) == -1) {
            return 1;
        }
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


typedef struct {
    char **line;
    int    count;
    int    cap;
} Doc;

static struct termios ed_saved_term;
static int ed_raw = 0;

static int ed_raw_on(void)
{
    if (tcgetattr(0, &ed_saved_term) == -1) { perror("tcgetattr"); return -1; }

    struct termios t = ed_saved_term;
    t.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_oflag &= (tcflag_t)~(OPOST);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;

    if (tcsetattr(0, TCSAFLUSH, &t) == -1) { perror("tcsetattr"); return -1; }
    ed_raw = 1;
    return 0;
}

static void ed_raw_off(void)
{
    if (!ed_raw) return;
    if (tcsetattr(0, TCSAFLUSH, &ed_saved_term) == -1) perror("tcsetattr");
    ed_raw = 0;
}

static void doc_free(Doc *d)
{
    for (int i = 0; i < d->count; i++) free(d->line[i]);
    free(d->line);
    d->line = NULL;
    d->count = d->cap = 0;
}

static int doc_push(Doc *d, const char *src, size_t n)
{
    if (d->count == d->cap) {
        int cap = d->cap ? d->cap * 2 : 32;
        char **nl = realloc(d->line, (size_t)cap * sizeof *nl);
        if (!nl) { perror("realloc"); return -1; }
        d->line = nl;
        d->cap = cap;
    }
    char *copy = malloc(n + 1);
    if (!copy) { perror("malloc"); return -1; }
    memcpy(copy, src, n);
    copy[n] = '\0';
    d->line[d->count++] = copy;
    return 0;
}

static int doc_load(Doc *d)
{
    size_t len;
    char *buf = ed_slurp(&len);
    if (!buf) return -1;

    d->line = NULL; d->count = 0; d->cap = 0;

    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && buf[j] != '\n') j++;
        if (doc_push(d, buf + i, j - i) == -1) { free(buf); doc_free(d); return -1; }
        i = j + 1;
    }
    if (d->count == 0 && doc_push(d, "", 0) == -1) { free(buf); doc_free(d); return -1; }

    free(buf);
    return 0;
}

static int doc_store(Doc *d)
{
    size_t total = 0;
    for (int i = 0; i < d->count; i++) total += strlen(d->line[i]) + 1;

    char *buf = malloc(total ? total : 1);
    if (!buf) { perror("malloc"); return -1; }

    size_t p = 0;
    for (int i = 0; i < d->count; i++) {
        size_t n = strlen(d->line[i]);
        memcpy(buf + p, d->line[i], n);
        p += n;
        buf[p++] = '\n';
    }

    int rc = ed_save(buf, p);
    free(buf);
    return rc;
}


typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} Screen;

static void sc_add(Screen *s, const char *src, size_t n)
{
    if (s->len + n > s->cap) {
        size_t cap = (s->cap ? s->cap : 4096);
        while (cap < s->len + n) cap *= 2;
        char *nb = realloc(s->buf, cap);
        if (!nb) return;
        s->buf = nb;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, src, n);
    s->len += n;
}

static void sc_str(Screen *s, const char *t) { sc_add(s, t, strlen(t)); }

static void ed_winsize(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0) {
        *rows = 24; *cols = 80;
    } else {
        *rows = ws.ws_row; *cols = ws.ws_col;
    }
}

static void ed_draw(Doc *d, const char *path, int cy, int cx, int rowoff, int dirty)
{
    int rows, cols;
    ed_winsize(&rows, &cols);

    int body = rows - 2;
    Screen s = {NULL, 0, 0};
    char tmp[256];

    sc_str(&s, "\x1b[?25l\x1b[H\x1b[2J");

    snprintf(tmp, sizeof tmp, "\x1b[7m %-*.*s \x1b[m\r\n", cols - 2, cols - 2, path);
    sc_str(&s, tmp);

    for (int y = 0; y < body; y++) {
        int i = y + rowoff;
        if (i < d->count) {
            size_t n = strlen(d->line[i]);
            if ((int)n > cols) n = (size_t)cols;
            sc_add(&s, d->line[i], n);
        } else {
            sc_str(&s, "\x1b[38;2;107;126;168m~\x1b[m");
        }
        sc_str(&s, "\r\n");
    }

    snprintf(tmp, sizeof tmp,
             "\x1b[7m %d/%d  %s \x1b[m\x1b[K  ^O save   ^X exit",
             cy + 1, d->count, dirty ? "modified" : "saved");
    sc_str(&s, tmp);

    snprintf(tmp, sizeof tmp, "\x1b[%d;%dH\x1b[?25h", cy - rowoff + 2, cx + 1);
    sc_str(&s, tmp);

    if (s.len && write(1, s.buf, s.len) == -1) perror("write");
    free(s.buf);
}

static int line_insert(Doc *d, int at, const char *src, size_t n)
{
    if (doc_push(d, "", 0) == -1) return -1;
    for (int i = d->count - 1; i > at; i--) d->line[i] = d->line[i - 1];
    char *copy = malloc(n + 1);
    if (!copy) { perror("malloc"); return -1; }
    memcpy(copy, src, n);
    copy[n] = '\0';
    d->line[at] = copy;
    return 0;
}

static void line_remove(Doc *d, int at)
{
    free(d->line[at]);
    for (int i = at; i < d->count - 1; i++) d->line[i] = d->line[i + 1];
    d->count--;
}

static int line_set(Doc *d, int at, const char *src, size_t n)
{
    char *copy = malloc(n + 1);
    if (!copy) { perror("malloc"); return -1; }
    memcpy(copy, src, n);
    copy[n] = '\0';
    free(d->line[at]);
    d->line[at] = copy;
    return 0;
}


static int ed_visual(const char *path)
{
    Doc d;
    if (doc_load(&d) == -1) return 1;
    if (ed_raw_on() == -1) { doc_free(&d); return 1; }

    int cy = 0, cx = 0, rowoff = 0, dirty = 0, rc = 0;

    for (;;) {
        int rows, cols;
        ed_winsize(&rows, &cols);
        int body = rows - 2;

        if (cy < rowoff) rowoff = cy;
        if (cy >= rowoff + body) rowoff = cy - body + 1;

        ed_draw(&d, path, cy, cx, rowoff, dirty);

        char c;
        ssize_t n = read(0, &c, 1);
        if (n == -1 && errno == EINTR) continue;
        if (n != 1) break;

        if (c == 24) break;

        if (c == 15) {
            if (doc_store(&d) == -1) { rc = 1; break; }
            dirty = 0;
            continue;
        }

        if (c == 27) {
            char seq[2];
            if (read(0, &seq[0], 1) != 1) continue;
            if (read(0, &seq[1], 1) != 1) continue;
            if (seq[0] != '[') continue;
            if (seq[1] == 'A' && cy > 0) cy--;
            else if (seq[1] == 'B' && cy < d.count - 1) cy++;
            else if (seq[1] == 'C') cx++;
            else if (seq[1] == 'D' && cx > 0) cx--;
            int ll = (int)strlen(d.line[cy]);
            if (cx > ll) cx = ll;
            continue;
        }

        if (c == '\r' || c == '\n') {
            char *cur = d.line[cy];
            size_t ll = strlen(cur);
            if ((size_t)cx > ll) cx = (int)ll;
            if (line_insert(&d, cy + 1, cur + cx, ll - (size_t)cx) == -1) { rc = 1; break; }
            if (line_set(&d, cy, cur, (size_t)cx) == -1) { rc = 1; break; }
            cy++; cx = 0; dirty = 1;
            continue;
        }

        if (c == 127 || c == 8) {
            char *cur = d.line[cy];
            size_t ll = strlen(cur);
            if (cx > 0) {
                char *nb = malloc(ll);
                if (!nb) { perror("malloc"); rc = 1; break; }
                memcpy(nb, cur, (size_t)cx - 1);
                memcpy(nb + cx - 1, cur + cx, ll - (size_t)cx);
                int ok = line_set(&d, cy, nb, ll - 1);
                free(nb);
                if (ok == -1) { rc = 1; break; }
                cx--;
            } else if (cy > 0) {
                size_t pl = strlen(d.line[cy - 1]);
                char *nb = malloc(pl + ll + 1);
                if (!nb) { perror("malloc"); rc = 1; break; }
                memcpy(nb, d.line[cy - 1], pl);
                memcpy(nb + pl, cur, ll);
                int ok = line_set(&d, cy - 1, nb, pl + ll);
                free(nb);
                if (ok == -1) { rc = 1; break; }
                line_remove(&d, cy);
                cy--; cx = (int)pl;
            }
            dirty = 1;
            continue;
        }

        if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
            char *cur = d.line[cy];
            size_t ll = strlen(cur);
            if ((size_t)cx > ll) cx = (int)ll;
            char *nb = malloc(ll + 2);
            if (!nb) { perror("malloc"); rc = 1; break; }
            memcpy(nb, cur, (size_t)cx);
            nb[cx] = c;
            memcpy(nb + cx + 1, cur + cx, ll - (size_t)cx);
            int ok = line_set(&d, cy, nb, ll + 1);
            free(nb);
            if (ok == -1) { rc = 1; break; }
            cx++; dirty = 1;
        }
    }

    ed_raw_off();
    if (write(1, "\x1b[2J\x1b[H", 7) == -1) perror("write");
    doc_free(&d);
    return rc;
}

static int ed_open(const char *path)
{
    if (!path || !*path) {
        printf("  usage: o <file>\n");
        return 1;
    }

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        perror("open");
        return 1;
    }

    ed_close();
    ed_fd = fd;
    printf("  %s%s%s  fd %d\n", C_BRAND, path, C_OFF, fd);
    return 0;
}

int cmd_edit(int argc, char **argv)
{
    char line[2048];
    char path[sizeof line] = "";
    int status = 0;

    if (argc >= 2 && strcmp(argv[1], "-v") == 0) {
        if (argc < 3) { printf("  usage: edit -v <file>\n"); return 1; }
        if (ed_open(argv[2]) != 0) return 1;
        snprintf(path, sizeof path, "%s", argv[2]);
        status = ed_visual(path);
        ed_close();
        return status;
    }

    if (argc >= 2) {
        if (ed_open(argv[1]) != 0) return 1;
        snprintf(path, sizeof path, "%s", argv[1]);
    }

    for (;;) {
        printf("  %sed%s %s ", C_BRAND, C_OFF, ICON_PROMPT);
        fflush(stdout);

        if (!fgets(line, sizeof line, stdin)) {
            if (ferror(stdin) && errno == EINTR) {
                clearerr(stdin);
                printf("\n");
                continue;
            }
            break;
        }

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
        if (strcmp(cmd, "o") == 0) {
            status = ed_open(arg);
            if (status == 0) snprintf(path, sizeof path, "%s", arg);
            continue;
        }

        if (ed_fd == -1) {
            printf("  %sno file open%s  use: o <file>\n", C_WARN, C_OFF);
            status = 1;
            continue;
        }

        if (strcmp(cmd, "v") == 0) status = ed_visual(path);
        else if (strcmp(cmd, "p") == 0) status = ed_print(arg);
        else if (strcmp(cmd, "a") == 0) status = ed_append(arg);
        else if (strcmp(cmd, "d") == 0) status = ed_delete(arg);
        else if (strcmp(cmd, "i") == 0) status = ed_insert(arg);
        else if (strcmp(cmd, "s") == 0) status = ed_search(arg);
        else { printf("  %s?%s o p a d i s v q\n", C_MUTED, C_OFF); status = 1; }
    }

    ed_close();
    return status;
}
