#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int inject_read_eintr;
static int inject_write_eintr;
static int inject_write_zero;
static int inject_write_short;
static ssize_t checked_read(int fd, void *buf, size_t count);
static ssize_t checked_write(int fd, const void *buf, size_t count);
#define read checked_read
#define write checked_write
#include "../cat_editor.c"
#undef read
#undef write

static ssize_t checked_read(int fd, void *buf, size_t count)
{
    if (inject_read_eintr) { inject_read_eintr = 0; errno = EINTR; return -1; }
    return read(fd, buf, count);
}

static ssize_t checked_write(int fd, const void *buf, size_t count)
{
    if (inject_write_eintr) { inject_write_eintr = 0; errno = EINTR; return -1; }
    if (inject_write_zero) { inject_write_zero = 0; return 0; }
    if (inject_write_short && count > 1) { inject_write_short = 0; count = 1; }
    return write(fd, buf, count);
}

int main(void)
{
    char path[] = "/tmp/moon-editor-errors-XXXXXX";
    ed_fd = mkstemp(path);
    assert(ed_fd >= 0);
    unlink(path);
    assert(write(ed_fd, "alpha", 5) == 5);
    size_t len = 0;
    inject_read_eintr = 1;
    char *contents = ed_slurp(&len);
    assert(contents && len == 5 && strcmp(contents, "alpha") == 0);
    free(contents);

    assert(lseek(ed_fd, 0, SEEK_SET) == 0);
    inject_write_eintr = 1;
    inject_write_short = 1;
    assert(ed_write("bravo", 5) == 0);
    assert(lseek(ed_fd, 0, SEEK_SET) == 0);
    char result[6] = {0};
    assert(read(ed_fd, result, 5) == 5 && strcmp(result, "bravo") == 0);
    inject_write_zero = 1;
    assert(ed_write("x", 1) == -1);
    ed_close();

    Doc d = {0};
    assert(doc_push(&d, "first", 5) == 0);
    assert(doc_push(&d, "last", 4) == 0);
    assert(line_insert(&d, 1, "middle", 6) == 0);
    assert(d.count == 3 && strcmp(d.line[1], "middle") == 0);
    doc_free(&d);
    puts("editor error cases passed");
    return 0;
}
