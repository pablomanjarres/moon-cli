/**
 * ====================================================================================
 * ui.c — THE VISUAL LAYER
 * ====================================================================================
 * All the art lives here: the gradient logo, the boxes, the prompt and the
 * strace-style syscall tracing.
 *
 * If you have never seen ANSI codes: they are magic strings the terminal does not
 * print, it interprets them as "change color". The \033[38;2;R;G;Bm form picks an
 * exact 24-bit color, and \033[0m goes back to normal.
 */

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>

/* strerrorname_np() gives you "ENOENT" instead of just the message. It exists in
 * glibc >= 2.32; if it is missing we fall back to printing the errno number. */
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#  if __GLIBC_PREREQ(2, 32)
#    define MOON_HAS_ERRNAME 1
#  endif
#endif

/* Layout widths. Change these and the whole shell re-lays itself out. */
#define BOX_INNER   62   /* inner width of a box (between │ and │)     */
#define BOX_KEY_W   18   /* width of the "key" column inside boxes     */
#define TRACE_COL   52   /* column where the  →  arrow lines up        */
#define SECTION_W   56   /* total width of ┌ title ──── headers        */
#define ROW_NAME_W  12   /* width of the "name" column in listings     */

/* ====================================================================================
 * 1. COLOR
 * ==================================================================================== */

static int g_color = 1;

static const char *const PALETTE[UI_COLOR_COUNT] = {
    [UI_OFF]    = "\033[0m",
    [UI_G1]     = "\033[38;2;237;239;247m",  /* #EDEFF7  moon white */
    [UI_G2]     = "\033[38;2;214;222;240m",  /* #D6DEF0  moonlight  */
    [UI_G3]     = "\033[38;2;180;196;228m",  /* #B4C4E4  brand      */
    [UI_G4]     = "\033[38;2;143;163;204m",  /* #8FA3CC             */
    [UI_G5]     = "\033[38;2;107;126;168m",  /* #6B7EA8  borders    */
    [UI_G6]     = "\033[38;2;76;92;134m",    /* #4C5C86  night      */
    [UI_ACCENT] = "\033[38;2;233;196;106m",  /* #E9C46A  moon gold  */
    [UI_INFO]   = "\033[38;2;126;166;217m",  /* #7EA6D9             */
    [UI_WARN]   = "\033[38;2;221;166;87m",   /* #DDA657             */
    [UI_ERR]    = "\033[38;2;224;108;117m",  /* #E06C75             */
    [UI_MUTED]  = "\033[38;2;139;147;167m",  /* #8B93A7  moon dust  */
    [UI_DIM]    = "\033[2m",
    [UI_BOLD]   = "\033[1m",
    [UI_TITLE]  = "\033[1;97m",
};

const char *ui_c(UiColor c)
{
    if (!g_color || c < 0 || c >= UI_COLOR_COUNT || PALETTE[c] == NULL)
        return "";
    return PALETTE[c];
}

int ui_has_color(void) { return g_color; }

/**
 * Any 24-bit color, built on the fly.
 *
 * The string has to survive until printf reads it, so it cannot be a local. We keep
 * a small rotating set of buffers instead of a single static one: that way a printf
 * with several ui_rgb() arguments still works, because each call lands in a
 * different slot. Four is plenty; past that the oldest one gets reused.
 */
const char *ui_rgb(int r, int g, int b)
{
    static char bufs[4][32];
    static int slot = 0;

    if (!g_color) return "";

    if (r < 0)   r = 0;
    if (r > 255) r = 255;
    if (g < 0)   g = 0;
    if (g > 255) g = 255;
    if (b < 0)   b = 0;
    if (b > 255) b = 255;

    slot = (slot + 1) % 4;
    snprintf(bufs[slot], sizeof(bufs[slot]), "\033[38;2;%d;%d;%dm", r, g, b);
    return bufs[slot];
}

/**
 * Decide whether we paint color at all:
 *  - MOON_FORCE_COLOR=1  -> always yes (handy for `./moon | less -R` or for tests)
 *  - NO_COLOR present    -> always no  (the https://no-color.org convention)
 *  - otherwise           -> only when stdout is a real terminal (isatty)
 */
void ui_init(void)
{
    if (getenv("MOON_FORCE_COLOR")) { g_color = 1; return; }
    if (getenv("NO_COLOR"))         { g_color = 0; return; }
    g_color = isatty(STDOUT_FILENO) ? 1 : 0;
}

/* ====================================================================================
 * 2. MEASURING TEXT
 * ====================================================================================
 * To align columns we need the *visible* width: ANSI escapes take no space on screen,
 * and neither do the extra bytes of a UTF-8 character. In UTF-8 the continuation
 * bytes start with 10xxxxxx (0x80..0xBF), so we simply do not count those.
 */
size_t ui_width(const char *s)
{
    size_t w = 0;
    while (*s) {
        if (*s == '\033') {                  /* skip a \033[...m sequence */
            while (*s && *s != 'm') s++;
            if (*s) s++;
            continue;
        }
        if (((unsigned char)*s & 0xC0) != 0x80)
            w++;
        s++;
    }
    return w;
}

static void pad(size_t n) { while (n-- > 0) fputc(' ', stdout); }

static void repeat(const char *glyph, int n)
{
    for (int i = 0; i < n; i++) fputs(glyph, stdout);
}

/* ====================================================================================
 * 3. LOGO AND WELCOME BLOCKS
 * ==================================================================================== */

/* Block font: every row gets a different shade of the moonlight ramp. */
static const char *const LOGO[6] = {
    "███╗   ███╗ ██████╗  ██████╗ ███╗   ██╗",
    "████╗ ████║██╔═══██╗██╔═══██╗████╗  ██║",
    "██╔████╔██║██║   ██║██║   ██║██╔██╗ ██║",
    "██║╚██╔╝██║██║   ██║██║   ██║██║╚██╗██║",
    "██║ ╚═╝ ██║╚██████╔╝╚██████╔╝██║ ╚████║",
    "╚═╝     ╚═╝ ╚═════╝  ╚═════╝ ╚═╝  ╚═══╝",
};

static const UiColor LOGO_TONE[6] = { UI_G1, UI_G2, UI_G3, UI_G4, UI_G5, UI_G6 };

void ui_logo(void)
{
    putchar('\n');
    for (int i = 0; i < 6; i++)
        printf("  %s%s%s\n", ui_c(LOGO_TONE[i]), LOGO[i], C_OFF);

    printf("  %sa shell that shows you the system calls%s  %s%s%s  %sv%s%s\n\n",
           C_MUTED, C_OFF,
           C_DIM, ICON_DOT, C_OFF,
           C_DIM, MOON_VERSION, C_OFF);
}

/* One-line version of the logo, so `help` does not repeat the whole ASCII block. */
void ui_title_line(void)
{
    printf("  %s%smoon%s  %sa shell that shows you the system calls%s  %s%s%s  %sv%s%s\n",
           C_BOLD, C_BRAND, C_OFF,
           C_MUTED, C_OFF,
           C_DIM, ICON_DOT, C_OFF,
           C_DIM, MOON_VERSION, C_OFF);
}

void ui_welcome(void)
{
    ui_logo();
    printf("  %s%s%s  %stype%s %shelp%s %sto list the commands%s  %s%s%s  %sexit%s %sor Ctrl+D to quit%s\n\n",
           C_BRAND, ICON_ARROW, C_OFF,
           C_MUTED, C_OFF,
           C_BRAND, C_OFF,
           C_MUTED, C_OFF,
           C_DIM, ICON_DOT, C_OFF,
           C_BRAND, C_OFF,
           C_MUTED, C_OFF);
}

void ui_farewell(void)
{
    printf("\n  %s%s%s  %sgood night%s\n\n", C_BRAND, ICON_MOON, C_OFF, C_MUTED, C_OFF);
}

/* ====================================================================================
 * 4. PROMPT
 * ====================================================================================
 * Looks like:      moon  ~/Projects/OS  ❯
 * and when the last command failed:
 *                  moon  ~/Projects/OS  ✖1  ❯
 */

/* Shortened current directory: $HOME becomes "~", and very long paths keep only the
 * last two folders behind an ellipsis. */
static void short_cwd(char *out, size_t out_sz)
{
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {   /* syscall: getcwd(2) */
        snprintf(out, out_sz, "?");
        return;
    }

    const char *home = getenv("HOME");
    size_t hlen = home ? strlen(home) : 0;
    char tilde[4096];

    /* Careful with the edge case: "/home/pablito" must not turn into "~to" just
     * because it starts like "/home/pablo". The prefix must end at '/' or at '\0'. */
    if (hlen > 0 && strncmp(cwd, home, hlen) == 0 &&
        (cwd[hlen] == '\0' || cwd[hlen] == '/')) {
        snprintf(tilde, sizeof(tilde), "~%s", cwd + hlen);
    } else {
        snprintf(tilde, sizeof(tilde), "%s", cwd);
    }

    if (strlen(tilde) <= 32) {
        snprintf(out, out_sz, "%s", tilde);
        return;
    }
    /* Too long: walk back to the second-to-last slash and cut there. */
    const char *p = tilde + strlen(tilde);
    int slashes = 0;
    while (p > tilde && slashes < 2) {
        p--;
        if (*p == '/') slashes++;
    }
    snprintf(out, out_sz, "…%s", p);
}

void ui_prompt(int last_status)
{
    char cwd[128];
    short_cwd(cwd, sizeof(cwd));

    printf("\n  %s%smoon%s %s%s%s", C_BOLD, C_BRAND, C_OFF, C_MUTED, cwd, C_OFF);
    if (last_status != 0)
        printf(" %s%s%d%s", C_ERR, ICON_FAIL, last_status, C_OFF);
    printf(" %s%s%s ", C_LIGHT, ICON_PROMPT, C_OFF);
    fflush(stdout);   /* required: printf does not flush without a '\n' */
}

/* ====================================================================================
 * 5. SMALL PIECES
 * ==================================================================================== */

void ui_blank(void) { putchar('\n'); }

void ui_section(const char *title)
{
    int used = (int)ui_width(title) + 3;              /* "┌ " + title + " " */
    int fill = SECTION_W - used;
    if (fill < 0) fill = 0;

    printf("  %s┌%s %s%s%s%s ", C_DARK, C_OFF, C_BOLD, C_BRAND, title, C_OFF);
    printf("%s", C_DIM);
    repeat("─", fill);
    printf("%s\n", C_OFF);
}

void ui_divider(void)
{
    printf("  %s", C_DIM);
    repeat("─", 22);
    printf("%s %s%s%s %s", C_OFF, C_BRAND, ICON_MOON, C_OFF, C_DIM);
    repeat("─", 22);
    printf("%s\n", C_OFF);
}

/* Shared engine behind the status messages (✔ ✖ ⚠ ℹ). */
static void status_line(const char *icon, UiColor icon_color, UiColor text_color,
                        const char *fmt, va_list ap)
{
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    printf("  %s%s%s  %s%s%s\n", ui_c(icon_color), icon, C_OFF,
           ui_c(text_color), msg, C_OFF);
}

void ui_ok(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    status_line(ICON_OK, UI_G3, UI_OFF, fmt, ap);
    va_end(ap);
}

void ui_fail(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    status_line(ICON_FAIL, UI_ERR, UI_ERR, fmt, ap);
    va_end(ap);
}

void ui_warn(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    status_line(ICON_WARN, UI_WARN, UI_OFF, fmt, ap);
    va_end(ap);
}

void ui_info(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    status_line(ICON_INFO, UI_INFO, UI_OFF, fmt, ap);
    va_end(ap);
}

void ui_hint(const char *fmt, ...)
{
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("     %s%s%s\n", C_MUTED, msg, C_OFF);
}

void ui_row(const char *name, const char *desc)
{
    size_t w = ui_width(name);
    printf("    %s%s%s", C_BRAND, name, C_OFF);
    pad(w < ROW_NAME_W ? ROW_NAME_W - w : 1);
    printf("%s%s%s\n", C_MUTED, desc, C_OFF);
}

/* ====================================================================================
 * 6. BOXES
 * ==================================================================================== */

void ui_box_top(const char *title)
{
    printf("  %s╭", C_DARK);
    if (title && *title) {
        int fill = BOX_INNER - 3 - (int)ui_width(title);
        if (fill < 0) fill = 0;
        printf("─%s %s%s%s%s ", C_OFF, C_BOLD, C_BRAND, title, C_OFF);
        printf("%s", C_DARK);
        repeat("─", fill);
    } else {
        repeat("─", BOX_INNER);
    }
    printf("╮%s\n", C_OFF);
}

/* Prints one content line inside the box, padded on the right. */
static void box_body(const char *content)
{
    size_t w = ui_width(content);
    printf("  %s│%s %s", C_DARK, C_OFF, content);
    pad(w + 2 < BOX_INNER ? BOX_INNER - 2 - w : 0);
    printf(" %s│%s\n", C_DARK, C_OFF);
}

void ui_box_line(const char *fmt, ...)
{
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    box_body(msg);
}

void ui_box_kv(const char *key, const char *fmt, ...)
{
    char val[512], content[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(val, sizeof(val), fmt, ap);
    va_end(ap);

    size_t kw = ui_width(key);
    char spaces[BOX_KEY_W + 1];
    size_t n = kw < BOX_KEY_W ? BOX_KEY_W - kw : 1;
    memset(spaces, ' ', n);
    spaces[n] = '\0';

    snprintf(content, sizeof(content), "%s%s%s%s%s%s%s",
             C_MUTED, key, C_OFF, spaces, C_BOLD, val, C_OFF);
    box_body(content);
}

void ui_box_sep(void)
{
    printf("  %s├", C_DARK);
    repeat("─", BOX_INNER);
    printf("┤%s\n", C_OFF);
}

void ui_box_bottom(void)
{
    printf("  %s╰", C_DARK);
    repeat("─", BOX_INNER);
    printf("╯%s\n", C_OFF);
}

/* ====================================================================================
 * 7. SYSCALL TRACING
 * ==================================================================================== */

/* Visible width of the line ui_syscall() left open, so the arrow can line up. */
static size_t g_call_w = 0;

void ui_trace_begin(const char *cmd_name)
{
    int fill = SECTION_W - (int)ui_width(cmd_name) - 3;
    if (fill < 0) fill = 0;
    printf("  %s┌%s %s%s%s%s ", C_DARK, C_OFF, C_BOLD, C_ACCENT, cmd_name, C_OFF);
    printf("%s", C_DIM);
    repeat("─", fill);
    printf("%s\n", C_OFF);
}

/**
 * Opens a syscall line (no newline yet): the name in gold, the arguments in gray.
 * The line is closed by ui_ret_int / ui_ret_ptr / ui_ret_err.
 */
void ui_syscall(const char *fmt, ...)
{
    char call[768];
    va_list ap; va_start(ap, fmt);
    vsnprintf(call, sizeof(call), fmt, ap);
    va_end(ap);

    g_call_w = ui_width(call);

    /* Split "name" from "(args...)" so each gets its own color. */
    const char *paren = strchr(call, '(');
    printf("  %s│%s ", C_DARK, C_OFF);
    if (paren) {
        int name_len = (int)(paren - call);
        printf("%s%s%.*s%s", C_BOLD, C_ACCENT, name_len, call, C_OFF);
        printf("%s%s%s", C_MUTED, paren, C_OFF);
    } else {
        printf("%s%s%s", C_MUTED, call, C_OFF);
    }
}

/* Pads out to the arrow column and draws it. */
static void arrow(void)
{
    pad(g_call_w < TRACE_COL ? TRACE_COL - g_call_w : 1);
    printf(" %s%s%s ", C_DIM, ICON_ARROW, C_OFF);
}

void ui_ret_int(long value)
{
    arrow();
    printf("%s%s%ld%s\n", C_BOLD, C_BRAND, value, C_OFF);
}

void ui_ret_ptr(const void *p)
{
    arrow();
    printf("%s%s%p%s\n", C_BOLD, C_BRAND, p, C_OFF);
}

void ui_ret_err(void)
{
    int e = errno;
    arrow();
#ifdef MOON_HAS_ERRNAME
    const char *name = strerrorname_np(e);
    printf("%s%s-1%s %s%s %s(%s)%s\n",
           C_BOLD, C_ERR, C_OFF,
           C_ERR, name ? name : "errno", C_OFF, strerror(e), C_OFF);
#else
    printf("%s%s-1%s %s(errno %d: %s)%s\n",
           C_BOLD, C_ERR, C_OFF, C_MUTED, e, strerror(e), C_OFF);
#endif
}

void ui_trace_note(const char *fmt, ...)
{
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("  %s│%s   %s%s%s\n", C_DARK, C_OFF, C_DIM, msg, C_OFF);
}

void ui_trace_ok(const char *fmt, ...)
{
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("  %s└%s %s%s%s  %s\n", C_DARK, C_OFF, C_BRAND, ICON_OK, C_OFF, msg);
}

void ui_trace_fail(const char *fmt, ...)
{
    char msg[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    printf("  %s└%s %s%s%s  %s%s%s\n", C_DARK, C_OFF, C_ERR, ICON_FAIL, C_OFF,
           C_ERR, msg, C_OFF);
}
