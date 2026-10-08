#include "shell.h"
#include "huffman.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <poll.h>

static int ed_fd = -1;
static HuffmanJob *ed_job;
static int ed_job_decompress, ed_snapshot_ready;
static char ed_job_notice[160];

typedef struct {
    char text[4096];
    size_t len;
    int clipped;
    int matches;
} EdFeedback;

typedef struct {
    char command[2048];
    size_t used;
    int focused;
    EdFeedback feedback;
} EdBar;

static EdFeedback *ed_feedback = NULL;
static int ed_out(const char *buf, size_t len);
static void ed_report(const char *format, ...);
static void ed_error(const char *name);

enum { ED_VIEW = 2, ED_QUIT };
enum { ED_OPEN, ED_PRINT, ED_APPEND, ED_DELETE, ED_INSERT, ED_SEARCH, ED_CLOSE,
       ED_COMPRESS, ED_DECOMPRESS };

static int ed_execute(char *line, char *path, size_t path_size, int dirty, int *refresh);

static const struct {
    const char *name, *args, *description;
    int kind;
} ed_commands[] = {
    {"o", "file",   "open or create a file", ED_OPEN},
    {"p", "[n]",    "print all text or line n", ED_PRINT},
    {"a", "text",   "append a line", ED_APPEND},
    {"d", "n",      "delete line n", ED_DELETE},
    {"i", "n text", "insert a line at n", ED_INSERT},
    {"s", "word",   "find matching lines", ED_SEARCH},
    {"q", "",       "close the file and return to moon", ED_CLOSE},
    {"c", "out",    "compress the saved file in the background", ED_COMPRESS},
    {"u", "in out", "decompress to a new file in the background", ED_DECOMPRESS},
};

static void ed_commands_text(char *buf, size_t len, int compact)
{
    size_t off = 0;
    for (size_t i = 0; i < sizeof ed_commands / sizeof *ed_commands; i++) {
        int args = !compact || (compact == 2 && ed_commands[i].kind < ED_COMPRESS);
        int n = snprintf(buf + off, len - off, "%s%s%s%s",
                         i ? (compact ? " " : "  ") : "", ed_commands[i].name,
                         args && *ed_commands[i].args ? " " : "",
                         args ? ed_commands[i].args : "");
        if (n < 0 || (size_t)n >= len - off) break;
        off += (size_t)n;
    }
}

static void ed_help(void)
{
    ed_report("  editor commands:\n");
    for (size_t i = 0; i < sizeof ed_commands / sizeof *ed_commands; i++)
        ed_report("  %s %-6s  %s\n", ed_commands[i].name, ed_commands[i].args,
               ed_commands[i].description);
    ed_report("  v         full-screen view (^O save, ^L commands, ^X return)\n"
           "  status    show background job progress\n"
           "  cancel    cancel the background job\n"
           "  help / ?  show these commands\n");
}

static int ed_job_update(void)
{
    if (!ed_job) return 0;
    HuffmanStatus status = huffman_status(ed_job);
    char notice[sizeof ed_job_notice];
    const char *action = ed_job_decompress ? "decompression" : "compression";
    ed_snapshot_ready = status.snapshot_ready;
    if (status.done) {
        if (status.cancelled) snprintf(notice, sizeof notice, "%s cancelled", action);
        else if (status.error) snprintf(notice, sizeof notice, "%s: %s", action,
                                        strerror(status.error));
        else snprintf(notice, sizeof notice, "%s complete", action);
        huffman_destroy(ed_job);
        ed_job = NULL;
    } else {
        unsigned percent = status.total ? (unsigned)(100.0L * status.completed / status.total) : 0;
        snprintf(notice, sizeof notice, "%s %u%%", !ed_snapshot_ready ? "capturing snapshot" :
                 ed_job_decompress ? "decompressing" : "compressing",
                 percent > 99 ? 99 : percent);
    }
    int changed = strcmp(notice, ed_job_notice) != 0;
    snprintf(ed_job_notice, sizeof ed_job_notice, "%s", notice);
    return changed;
}

static int ed_wait_input(void)
{
    struct pollfd fds[2] = {{0, POLLIN, 0}, {-1, POLLIN, 0}};
    if (ed_job) fds[1].fd = huffman_event_fd(ed_job);
    int ready;
    do { ready = poll(fds, ed_job ? 2 : 1, -1); } while (ready == -1 && errno == EINTR);
    if (ready == -1) { ed_error("poll"); return -1; }
    if (fds[1].revents) ed_job_update();
    return fds[0].revents ? 1 : 0;
}

static int ed_snapshot_busy(void)
{
    ed_job_update();
    if (!ed_job || ed_snapshot_ready) return 0;
    ed_report("snapshot in progress; retry file command or save\n");
    return 1;
}

static int ed_start_job(char *arg, int decompress)
{
    ed_job_update();
    if (ed_job) { ed_report("a background job is already running\n"); return 1; }
    char *args[4];
    int count = parse_line(arg, args, 4);
    if (count != (decompress ? 2 : 1)) {
        ed_report("usage: %s\n", decompress ? "u <input> <output>" : "c <output>");
        return 1;
    }
    int input = decompress ? open(args[0], O_RDONLY) : ed_fd;
    if (input == -1) { ed_error("open"); return 1; }
    ed_job = huffman_start(input, args[decompress ? 1 : 0], decompress, 4);
    int saved = errno;
    if (decompress) close(input);
    if (!ed_job) { errno = saved; ed_error("huffman"); return 1; }
    ed_job_decompress = decompress;
    ed_snapshot_ready = 0;
    snprintf(ed_job_notice, sizeof ed_job_notice, "%s 0%%",
             decompress ? "decompressing" : "compressing");
    ed_report("%s\n", ed_job_notice);
    return 0;
}

static void ed_stop_job(void)
{
    if (ed_job) {
        huffman_cancel(ed_job);
        huffman_destroy(ed_job);
        ed_job = NULL;
    }
    ed_job_notice[0] = '\0';
}

static ssize_t ed_read(int fd, void *buf, size_t len)
{
    ssize_t n;
    do { n = read(fd, buf, len); } while (n == -1 && errno == EINTR);
    return n;
}

static int ed_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n == -1 && errno == EINTR) continue;
        if (n <= 0) {
            if (n == 0) errno = EIO;
            ed_error("write");
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

enum { ED_ESC_PREFIX = 1, ED_ESC_CSI, ED_ESC_PARAMS };
enum { ED_KEY_CANCEL, ED_KEY_SEQUENCE, ED_KEY_WAIT, ED_KEY_REPLAY };

typedef struct {
    int state, late;
    unsigned char replay[32];
    size_t used, next;
} EdEscape;

static int ed_escape_sequence(char *key, int *pending, EdEscape *escape)
{
    for (size_t bytes = 0; bytes < sizeof escape->replay; bytes++) {
        unsigned char c;
        if (*pending != -1) { c = (unsigned char)*pending; *pending = -1; }
        else {
            struct pollfd input = {0, POLLIN, 0};
            int ready;
            int timeout = escape->state == ED_ESC_PREFIX ? 250 : 30;
            do { ready = poll(&input, 1, timeout); } while (ready == -1 && errno == EINTR);
            if (ready == -1) { ed_error("poll"); return -1; }
            /* Keep the state so a delayed prefix/final byte cannot become text. */
            if (!ready) {
                if (escape->state != ED_ESC_PREFIX) return ED_KEY_WAIT;
                escape->late = 1;
                return ED_KEY_CANCEL;
            }
            ssize_t n = ed_read(0, &c, 1);
            if (n == -1) { ed_error("read"); return -1; }
            if (n != 1) {
                escape->state = 0;
                return escape->used ? ED_KEY_REPLAY : ED_KEY_CANCEL;
            }
        }
        if (escape->state == ED_ESC_PREFIX) {
            if (c == '[') {
                escape->state = ED_ESC_CSI;
                if (escape->late) escape->replay[escape->used++] = c;
                continue;
            }
        } else {
            if (escape->late) {
                if (escape->used == sizeof escape->replay || c < 0x20 || c > 0x7e) {
                    escape->state = 0;
                    *pending = c;
                    return ED_KEY_REPLAY;
                }
                escape->replay[escape->used++] = c;
            }
            if (c >= 0x40 && c <= 0x7e) {
                *key = escape->state == ED_ESC_CSI ? (char)c : '\0';
                escape->state = 0;
                if (escape->late && (*key < 'A' || *key > 'D')) return ED_KEY_REPLAY;
                escape->used = escape->next = 0;
                escape->late = 0;
                return ED_KEY_SEQUENCE;
            }
            if (c >= 0x20 && c <= 0x3f) { escape->state = ED_ESC_PARAMS; continue; }
        }
        escape->state = 0;
        *pending = c;
        return ED_KEY_CANCEL;
    }
    return ED_KEY_WAIT;
}

static int ed_close(void)
{
    if (ed_fd == -1) return 0;
    int rc = close(ed_fd);
    if (rc == -1) ed_error("close");
    ed_fd = -1;
    return rc;
}

static char *ed_slurp(size_t *len)
{
    off_t end = lseek(ed_fd, 0, SEEK_END);
    if (end == -1) { ed_error("lseek"); return NULL; }
    if (lseek(ed_fd, 0, SEEK_SET) == -1) { ed_error("lseek"); return NULL; }

    char *buf = malloc((size_t)end + 1);
    if (!buf) { ed_error("malloc"); return NULL; }

    size_t got = 0;
    while (got < (size_t)end) {
        ssize_t n = ed_read(ed_fd, buf + got, (size_t)end - got);
        if (n == -1) { ed_error("read"); free(buf); return NULL; }
        if (n == 0) break;
        got += (size_t)n;
    }

    buf[got] = '\0';
    *len = got;
    return buf;
}

static int ed_out(const char *buf, size_t len)
{
    if (ed_feedback) {
        size_t room = sizeof ed_feedback->text - ed_feedback->len - 1;
        size_t n = len < room ? len : room;
        memcpy(ed_feedback->text + ed_feedback->len, buf, n);
        ed_feedback->len += n;
        ed_feedback->text[ed_feedback->len] = '\0';
        if (n < len) ed_feedback->clipped = 1;
        return 0;
    }
    return ed_write_all(1, buf, len);
}

static void ed_report(const char *format, ...)
{
    char buf[512];
    va_list args;
    va_start(args, format);
    if (!ed_feedback) {
        vprintf(format, args);
        va_end(args);
        return;
    }
    int n = vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf && ed_feedback) ed_feedback->clipped = 1;
    ed_out(buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static void ed_error(const char *name)
{
    int saved = errno;
    if (ed_feedback) ed_report("%s: %s\n", name, strerror(saved));
    errno = saved;
    perror(name);
}

static char *ed_next_line(char *buf, size_t len, size_t *at, size_t *out_len)
{
    if (*at >= len) return NULL;
    size_t i = *at, j = i;
    while (j < len && buf[j] != '\n') j++;
    *out_len = j - i;
    *at = j < len ? j + 1 : j;
    return buf + i;
}

static char *ed_line(char *buf, size_t len, int n, size_t *out_len)
{
    if (n < 1) return NULL;
    size_t at = 0;
    char *line;
    for (int cur = 1; (line = ed_next_line(buf, len, &at, out_len)); cur++)
        if (cur == n) return line;
    return NULL;
}

static int ed_print(const char *arg)
{
    size_t len;
    char *buf = ed_slurp(&len);
    if (!buf) return 1;

    int rc = 0;
    if (!arg || !*arg) {
        if (ed_feedback) {
            ed_report("All file lines:\n");
            size_t at = 0, ll;
            int n = 1;
            char *line;
            while ((line = ed_next_line(buf, len, &at, &ll))) {
                ed_report("%d  ", n++);
                ed_out(line, ll);
                ed_out("\n", 1);
            }
        } else if (len) {
            rc = ed_out(buf, len) == -1;
            if (!rc && buf[len - 1] != '\n') rc = ed_out("\n", 1) == -1;
        }
    } else {
        size_t ll;
        char *l = ed_line(buf, len, atoi(arg), &ll);
        if (!l) {
            ed_report("  no such line\n");
            rc = 1;
        } else {
            if (ed_feedback) ed_report("%d  ", atoi(arg));
            rc = ed_out(l, ll) == -1 || ed_out("\n", 1) == -1;
        }
    }

    free(buf);
    return rc;
}

static int ed_write(const char *buf, size_t len)
{
    return ed_write_all(ed_fd, buf, len);
}

static int ed_append(const char *text)
{
    if (!text) text = "";

    off_t end = lseek(ed_fd, 0, SEEK_END);
    if (end == -1) { ed_error("lseek"); return 1; }

    if (end > 0) {
        char last;
        if (lseek(ed_fd, end - 1, SEEK_SET) == -1) { ed_error("lseek"); return 1; }
        ssize_t got = ed_read(ed_fd, &last, 1);
        if (got == -1) { ed_error("read"); return 1; }
        if (got != 1) {
            if (lseek(ed_fd, 0, SEEK_END) == -1) { ed_error("lseek"); return 1; }
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
    if (lseek(ed_fd, 0, SEEK_SET) == -1) { ed_error("lseek"); return -1; }
    if (ed_write(buf, len) == -1) return -1;
    if (ftruncate(ed_fd, (off_t)len) == -1) { ed_error("ftruncate"); return -1; }
    return 0;
}

static int ed_delete(const char *arg)
{
    if (!arg || !*arg) {
        ed_report("  usage: d <n>\n");
        return 1;
    }

    size_t len;
    char *buf = ed_slurp(&len);
    if (!buf) return 1;

    size_t ll;
    char *l = ed_line(buf, len, atoi(arg), &ll);
    if (!l) {
        ed_report("  no such line\n");
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
        ed_report("  usage: i <n> <text>\n");
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
        ed_report("  usage: i <n> <text>\n");
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
    if (!nb) { ed_error("malloc"); free(buf); return 1; }

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
        ed_report("  usage: s <word>\n");
        return 1;
    }

    size_t len;
    char *buf = ed_slurp(&len);
    if (!buf) return 1;

    int hits = 0;
    size_t at = 0, ll;
    int n = 1;
    char *line;

    while ((line = ed_next_line(buf, len, &at, &ll))) {
        char save = line[ll];
        line[ll] = '\0';
        if (strstr(line, word)) {
            ed_report("  %d  %s\n", n, line);
            hits++;
        }
        line[ll] = save;
        n++;
    }

    if (ed_feedback) ed_feedback->matches = hits;
    if (!hits) ed_report("  not found\n");

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
    if (tcgetattr(0, &ed_saved_term) == -1) { ed_error("tcgetattr"); return -1; }

    struct termios t = ed_saved_term;
    t.c_lflag &= (tcflag_t)~(ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_oflag &= (tcflag_t)~(OPOST);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;

    if (tcsetattr(0, TCSAFLUSH, &t) == -1) { ed_error("tcsetattr"); return -1; }
    ed_raw = 1;
    return 0;
}

static int ed_raw_off(void)
{
    if (!ed_raw) return 0;
    if (tcsetattr(0, TCSAFLUSH, &ed_saved_term) == -1) {
        ed_error("tcsetattr");
        return -1;
    }
    ed_raw = 0;
    return 0;
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
        if (!nl) { ed_error("realloc"); return -1; }
        d->line = nl;
        d->cap = cap;
    }
    char *copy = malloc(n + 1);
    if (!copy) { ed_error("malloc"); return -1; }
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

    size_t at = 0, line_len;
    char *line;
    while ((line = ed_next_line(buf, len, &at, &line_len)))
        if (doc_push(d, line, line_len) == -1) { free(buf); doc_free(d); return -1; }
    if (d->count == 0 && doc_push(d, "", 0) == -1) { free(buf); doc_free(d); return -1; }

    free(buf);
    return 0;
}

static int doc_store(Doc *d)
{
    size_t total = 0;
    for (int i = 0; i < d->count; i++) total += strlen(d->line[i]) + 1;

    char *buf = malloc(total ? total : 1);
    if (!buf) { ed_error("malloc"); return -1; }

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
    int failed;
} Screen;

static void sc_add(Screen *s, const char *src, size_t n)
{
    if (s->failed) return;
    if (n > SIZE_MAX - s->len) { errno = ENOMEM; ed_error("realloc"); s->failed = 1; return; }
    if (s->len + n > s->cap) {
        size_t cap = (s->cap ? s->cap : 4096);
        while (cap < s->len + n) {
            if (cap > SIZE_MAX / 2) { cap = s->len + n; break; }
            cap *= 2;
        }
        char *nb = realloc(s->buf, cap);
        if (!nb) { ed_error("realloc"); s->failed = 1; return; }
        s->buf = nb;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, src, n);
    s->len += n;
}

static void sc_str(Screen *s, const char *t) { sc_add(s, t, strlen(t)); }

static void sc_row(Screen *s, const char *text, int cols, int inverse, int next)
{
    size_t n = strlen(text);
    if (n > (size_t)cols) n = (size_t)cols;
    if (inverse) sc_str(s, "\x1b[7m");
    sc_add(s, text, n);
    sc_str(s, "\x1b[m\x1b[K");
    if (next) sc_str(s, "\r\n");
}

static int ed_result_rows(int rows, int cols, EdFeedback *feedback, int *clipped)
{
    int limit = rows > 8 ? 4 : rows > 5 ? rows - 5 : 0;
    int count = 0, cut = feedback->clipped;
    size_t at = 0, len;
    while (ed_next_line(feedback->text, feedback->len, &at, &len)) {
        count++;
        if (len + 8 > (size_t)cols) cut = 1;
    }
    if (count > limit) cut = 1;
    if (clipped) *clipped = cut;
    int total = count ? count + cut : 0;
    return total < limit ? total : limit;
}

static int ed_body_rows(int rows, int results)
{
    return rows > 4 + results ? rows - 4 - results : 0;
}

static void ed_winsize(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_row == 0 || ws.ws_col == 0) {
        *rows = 24; *cols = 80;
    } else {
        *rows = ws.ws_row; *cols = ws.ws_col;
    }
}

static int ed_draw(Doc *d, const char *path, int cy, int cx, int rowoff,
                   int dirty, const char *notice, EdBar *bar)
{
    int rows, cols;
    ed_winsize(&rows, &cols);

    int clipped;
    int results = ed_result_rows(rows, cols, &bar->feedback, &clipped);
    int body = ed_body_rows(rows, results);
    Screen s = {NULL, 0, 0, 0};
    char tmp[256];

    sc_str(&s, "\x1b[?25l\x1b[H\x1b[2J");

    const char *title = path;
    if (strlen(path) > (size_t)cols) {
        const char *base = strrchr(path, '/');
        if (base) title = base + 1;
    }
    if (rows > 3) sc_row(&s, title, cols, 1, 1);
    int digits = 1;
    for (int n = d->count; n >= 10 && digits < 10; n /= 10) digits++;
    int gutter = digits + 1;
    if (gutter >= cols) gutter = 0;

    for (int y = 0; y < body; y++) {
        int i = y + rowoff;
        if (i < d->count) {
            if (gutter) {
                snprintf(tmp, sizeof tmp, "%*d ", digits, i + 1);
                sc_str(&s, tmp);
            }
            size_t n = strlen(d->line[i]);
            if (n > (size_t)(cols - gutter)) n = (size_t)(cols - gutter);
            sc_add(&s, d->line[i], n);
        } else {
            sc_str(&s, "\x1b[38;2;107;126;168m~\x1b[m");
        }
        sc_str(&s, "\r\n");
    }

    size_t at = 0, len;
    for (int y = 0; y < results; y++) {
        if (clipped && y == results - 1) {
            if (bar->feedback.matches) snprintf(tmp, sizeof tmp,
                "%d matches; results clipped", bar->feedback.matches);
            else snprintf(tmp, sizeof tmp, "... results clipped");
        } else {
            char *line = ed_next_line(bar->feedback.text, bar->feedback.len, &at, &len);
            snprintf(tmp, sizeof tmp, "Result: %.*s", (int)len, line ? line : "");
        }
        sc_row(&s, tmp, cols, 0, 1);
    }
    if (rows > 2) {
        ed_commands_text(tmp, sizeof tmp, 0);
        if (strlen(tmp) > (size_t)cols) ed_commands_text(tmp, sizeof tmp, 2);
        if (strlen(tmp) > (size_t)cols) ed_commands_text(tmp, sizeof tmp, 1);
        sc_row(&s, tmp, cols, 1, 1);
    }
    if (notice) snprintf(tmp, sizeof tmp, "%s", notice);
    else if (*ed_job_notice) snprintf(tmp, sizeof tmp, "%s  ^L commands  ^O save", ed_job_notice);
    else snprintf(tmp, sizeof tmp, "^L commands  ^O save  ^X %s  %d/%d %s",
                  dirty ? "discard" : "exit", cy + 1, d->count, dirty ? "modified" : "saved");
    if (rows > 1) sc_row(&s, tmp, cols, 1, 1);
    const char *prefix = "Command: ";
    size_t room = cols > 9 ? (size_t)cols - 9 : (size_t)cols;
    size_t start = bar->used > room ? bar->used - room : 0;
    if (bar->focused) snprintf(tmp, sizeof tmp, "%s%s",
                              cols > 9 ? prefix : "", bar->command + start);
    else snprintf(tmp, sizeof tmp, "Editing; ^L commands");
    sc_row(&s, tmp, cols, bar->focused, 0);

    int cursor_row = bar->focused ? rows : body ? cy - rowoff + 2 : 1;
    int cursor_col = bar->focused ? (int)strlen(tmp) + 1 : cx + gutter + 1;
    if (cursor_col > cols) cursor_col = cols;
    snprintf(tmp, sizeof tmp, "\x1b[%d;%dH\x1b[?25h", cursor_row, cursor_col);
    sc_str(&s, tmp);

    int rc = s.failed ? -1 : ed_write_all(1, s.buf, s.len);
    free(s.buf);
    return rc;
}

static int line_insert(Doc *d, int at, const char *src, size_t n)
{
    char *copy = malloc(n + 1);
    if (!copy) { ed_error("malloc"); return -1; }
    memcpy(copy, src, n);
    copy[n] = '\0';
    if (d->count == d->cap) {
        int cap = d->cap ? d->cap * 2 : 32;
        char **nl = realloc(d->line, (size_t)cap * sizeof *nl);
        if (!nl) { ed_error("realloc"); free(copy); return -1; }
        d->line = nl;
        d->cap = cap;
    }
    for (int i = d->count; i > at; i--) d->line[i] = d->line[i - 1];
    d->line[at] = copy;
    d->count++;
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
    if (!copy) { ed_error("malloc"); return -1; }
    memcpy(copy, src, n);
    copy[n] = '\0';
    free(d->line[at]);
    d->line[at] = copy;
    return 0;
}


static int ed_visual(char *path, size_t path_size)
{
    Doc d;
    if (doc_load(&d) == -1) return 1;
    if (ed_raw_on() == -1) { doc_free(&d); return 1; }

    int cy = 0, cx = 0, rowoff = 0, dirty = 0, rc = 0;
    const char *notice = NULL;
    EdBar bar = {0};
    int pending = -1;
    EdEscape escape = {0};

    for (;;) {
        int rows, cols;
        ed_winsize(&rows, &cols);
        int body = ed_body_rows(rows, ed_result_rows(rows, cols, &bar.feedback, NULL));

        if (body == 0 || cy < rowoff) rowoff = cy;
        else if (cy >= rowoff + body) rowoff = cy - body + 1;

        if (ed_draw(&d, path, cy, cx, rowoff, dirty, notice, &bar) == -1) { rc = 1; break; }

        char c;
        ssize_t n;
        if (!escape.state && escape.next < escape.used) {
            c = (char)escape.replay[escape.next++]; n = 1;
            if (escape.next == escape.used) escape.used = escape.next = 0;
        } else if (pending != -1) { c = (char)pending; pending = -1; n = 1; }
        else {
            int ready = ed_wait_input();
            if (ready == -1) { rc = 1; break; }
            if (!ready) continue;
            n = ed_read(0, &c, 1);
        }
        if (n == -1) { ed_error("read"); rc = 1; break; }
        if (n == 0) break;

        if (c == 27 || escape.state) {
            char key = '\0';
            if (c == 27) escape = (EdEscape){.state = ED_ESC_PREFIX};
            else pending = (unsigned char)c;
            int sequence = ed_escape_sequence(&key, &pending, &escape);
            if (sequence == -1) { rc = 1; break; }
            if (sequence == ED_KEY_REPLAY) continue;
            if (sequence == ED_KEY_CANCEL) {
                bar.focused = 0;
                bar.used = 0; bar.command[0] = '\0';
                if (pending == -1 || pending == 27) continue;
                c = (char)pending; pending = -1;
            } else {
                if (sequence == ED_KEY_WAIT || bar.focused) continue;
                if (key == 'A' && cy > 0) cy--;
                else if (key == 'B' && cy < d.count - 1) cy++;
                else if (key == 'C') cx++;
                else if (key == 'D' && cx > 0) cx--;
                int ll = (int)strlen(d.line[cy]);
                if (cx > ll) cx = ll;
                continue;
            }
        }

        if (c == 24) break;

        if (c == 12) {
            bar.focused = !bar.focused;
            notice = NULL;
            continue;
        }
        notice = NULL;

        if (c == 15) {
            bar.feedback = (EdFeedback){0};
            ed_feedback = &bar.feedback;
            if (ed_snapshot_busy()) rc = 1;
            else if (doc_store(&d) == -1) rc = 1;
            else { dirty = 0; rc = 0; ed_report("saved file\n"); }
            ed_feedback = NULL;
            continue;
        }

        if (bar.focused) {
            if (c == '\r' || c == '\n') {
                if (!bar.used) continue;
                bar.feedback = (EdFeedback){0};
                ed_feedback = &bar.feedback;
                int refresh, status = ed_execute(bar.command, path, path_size, dirty, &refresh);
                bar.used = 0; bar.command[0] = '\0';
                if (status == ED_QUIT) { ed_feedback = NULL; rc = ED_QUIT; break; }
                if (status == ED_VIEW) bar.focused = 0;
                if (refresh) {
                    Doc next;
                    if (doc_load(&next) == -1) { ed_feedback = NULL; rc = 1; break; }
                    doc_free(&d); d = next;
                    if (refresh == 2) cy = cx = rowoff = 0;
                    if (cy >= d.count) cy = d.count - 1;
                    int len = (int)strlen(d.line[cy]);
                    if (cx > len) cx = len;
                }
                ed_feedback = NULL;
            } else if (c == 127 || c == 8) {
                if (bar.used) bar.command[--bar.used] = '\0';
            } else if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
                if (bar.used < sizeof bar.command - 1) {
                    bar.command[bar.used++] = c;
                    bar.command[bar.used] = '\0';
                }
            }
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
                if (!nb) { ed_error("malloc"); rc = 1; break; }
                memcpy(nb, cur, (size_t)cx - 1);
                memcpy(nb + cx - 1, cur + cx, ll - (size_t)cx);
                int ok = line_set(&d, cy, nb, ll - 1);
                free(nb);
                if (ok == -1) { rc = 1; break; }
                cx--;
            } else if (cy > 0) {
                size_t pl = strlen(d.line[cy - 1]);
                char *nb = malloc(pl + ll + 1);
                if (!nb) { ed_error("malloc"); rc = 1; break; }
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
            if (!nb) { ed_error("malloc"); rc = 1; break; }
            memcpy(nb, cur, (size_t)cx);
            nb[cx] = c;
            memcpy(nb + cx + 1, cur + cx, ll - (size_t)cx);
            int ok = line_set(&d, cy, nb, ll + 1);
            free(nb);
            if (ok == -1) { rc = 1; break; }
            cx++; dirty = 1;
        }
    }

    if (ed_raw_off() == -1) rc = 1;
    if (ed_write_all(1, "\x1b[2J\x1b[H", 7) == -1) rc = 1;
    doc_free(&d);
    return rc;
}

static int ed_open(const char *path)
{
    if (!path || !*path) {
        ed_report("  usage: o <file>\n");
        return 1;
    }

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        ed_error("open");
        return 1;
    }

    if (ed_close() == -1) {
        if (close(fd) == -1) ed_error("close");
        return 1;
    }
    ed_fd = fd;
    ed_report("  opened %s\n", path);
    return 0;
}

static char *ed_parse_command(char *line, char **arg)
{
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    while (*line == ' ' || *line == '\t') line++;
    *arg = line;
    while (**arg && **arg != ' ' && **arg != '\t') (*arg)++;
    if (**arg) {
        *(*arg)++ = '\0';
        while (**arg == ' ' || **arg == '\t') (*arg)++;
    }
    return line;
}

static int ed_execute(char *line, char *path, size_t path_size, int dirty, int *refresh)
{
    char *arg, *name = ed_parse_command(line, &arg);
    *refresh = 0;
    if (!*name) return 0;
    if (strcmp(name, "help") == 0 || strcmp(name, "?") == 0) { ed_help(); return 0; }
    if (strcmp(name, "status") == 0) {
        ed_job_update();
        ed_report("%s\n", *ed_job_notice ? ed_job_notice : "no background job");
        return 0;
    }
    if (strcmp(name, "cancel") == 0) {
        ed_job_update();
        if (ed_job) { huffman_cancel(ed_job); ed_report("cancellation requested\n"); }
        else ed_report("no background job\n");
        return 0;
    }
    int kind = -1;
    for (size_t i = 0; i < sizeof ed_commands / sizeof *ed_commands; i++)
        if (strcmp(name, ed_commands[i].name) == 0) kind = ed_commands[i].kind;
    if (kind == -1 && strcmp(name, "v") != 0) {
        ed_report("  unknown command: %s; use help\n", name);
        return 1;
    }
    if (dirty && kind != -1 && kind != ED_DECOMPRESS) {
        ed_report("save with ^O before file commands\n");
        return 1;
    }
    if (kind == ED_CLOSE) return ED_QUIT;
    if (kind == ED_DECOMPRESS) return ed_start_job(arg, 1);
    if (kind == ED_OPEN) {
        if (strlen(arg) >= path_size) { ed_report("file path too long\n"); return 1; }
        int rc = ed_open(arg);
        if (!rc) { snprintf(path, path_size, "%s", arg); *refresh = 2; }
        return rc;
    }
    if (ed_fd == -1) { ed_report("  no file open; use o <file>\n"); return 1; }
    if (kind == ED_COMPRESS) return ed_start_job(arg, 0);
    if (kind == -1) return ED_VIEW;
    if ((kind == ED_APPEND || kind == ED_DELETE || kind == ED_INSERT) && ed_snapshot_busy()) return 1;
    int rc;
    switch (kind) {
        case ED_PRINT: rc = ed_print(arg); break;
        case ED_APPEND: rc = ed_append(arg); break;
        case ED_DELETE: rc = ed_delete(arg); break;
        case ED_INSERT: rc = ed_insert(arg); break;
        default: rc = ed_search(arg); break;
    }
    if (!rc && (kind == ED_APPEND || kind == ED_DELETE || kind == ED_INSERT)) {
        *refresh = 1;
        ed_report("saved changes\n");
    }
    return rc;
}

static int ed_read_line(char *line, size_t size)
{
    size_t used = 0;
    int clipped = 0;
    for (;;) {
        char before[sizeof ed_job_notice];
        snprintf(before, sizeof before, "%s", ed_job_notice);
        int ready = ed_wait_input();
        if (ready == -1) return -1;
        if (strcmp(before, ed_job_notice) != 0) {
            ed_report("\n%s\n  %sed%s %s ", ed_job_notice, C_BRAND, C_OFF, ICON_PROMPT);
            fflush(stdout);
        }
        if (!ready) continue;
        char c;
        ssize_t n = ed_read(0, &c, 1);
        if (n == -1) { ed_error("read"); return -1; }
        if (!n) { line[used] = '\0'; return used ? 1 : 0; }
        if (c == '\n') {
            line[used] = '\0';
            if (clipped) { ed_report("command too long\n"); used = 0; clipped = 0; continue; }
            return 1;
        }
        if (used < size - 1) line[used++] = c;
        else clipped = 1;
    }
}

int cmd_edit(int argc, char **argv)
{
    char line[2048];
    char path[sizeof line] = "";
    int status = 0;

    if (argc >= 2) {
        if (strlen(argv[1]) >= sizeof path) { ed_report("file path too long\n"); return 1; }
        if (ed_open(argv[1]) != 0) return 1;
        snprintf(path, sizeof path, "%s", argv[1]);
        status = ed_visual(path, sizeof path);
        if (status == ED_QUIT) status = 0;
        ed_stop_job();
        if (ed_close() == -1) status = 1;
        return status;
    }

    ed_help();
    for (;;) {
        printf("  %sed%s %s ", C_BRAND, C_OFF, ICON_PROMPT);
        fflush(stdout);

        int read_status = ed_read_line(line, sizeof line);
        if (read_status <= 0) { if (read_status == -1) status = 1; break; }

        int refresh;
        status = ed_execute(line, path, sizeof path, 0, &refresh);
        if (status == ED_QUIT) { status = 0; break; }
        if (status == ED_VIEW) {
            status = ed_visual(path, sizeof path);
            if (status == ED_QUIT) { status = 0; break; }
        }
    }

    ed_stop_job();
    if (ed_close() == -1) status = 1;
    return status;
}
