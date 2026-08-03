#ifndef UI_H
#define UI_H

/**
 * ====================================================================================
 * ui.h — PRESENTATION LAYER (everything the shell looks like)
 * ====================================================================================
 * All the visuals live here. Commands should never print raw escape codes: they call
 * these functions instead. Want to restyle the whole shell? Touch this file and ui.c.
 * Nothing else needs to know.
 *
 * Project rule:
 *   cat_*.c  -> logic (system calls)
 *   ui.c     -> how it looks
 *   main.c   -> the loop
 */

#include <stddef.h>

#define MOON_VERSION "0.1.0"

/* ------------------------------------------------------------------------------------
 * 1. PALETTE
 * ------------------------------------------------------------------------------------
 * 24-bit "truecolor" escapes: \033[38;2;R;G;Bm.
 * The moonlight ramp G1..G6 runs from moon white down into night blue, and it is
 * what paints the logo gradient.
 */
typedef enum {
    UI_OFF = 0,   /* reset every attribute */
    UI_G1,        /* moon white       #EDEFF7 */
    UI_G2,        /* moonlight        #D6DEF0 */
    UI_G3,        /* brand            #B4C4E4 */
    UI_G4,        /*                  #8FA3CC */
    UI_G5,        /* box borders      #6B7EA8 */
    UI_G6,        /* night            #4C5C86 */
    UI_ACCENT,    /* moon gold        #E9C46A  (syscall names) */
    UI_INFO,      /* night blue       #7EA6D9 */
    UI_WARN,      /* amber            #DDA657 */
    UI_ERR,       /* rose red         #E06C75 */
    UI_MUTED,     /* moon dust gray   #8B93A7  (secondary text) */
    UI_DIM,       /* dimmed */
    UI_BOLD,      /* bold */
    UI_TITLE,     /* bold white */
    UI_COLOR_COUNT
} UiColor;

/* Returns the ANSI sequence for a color, or "" when color is disabled. */
const char *ui_c(UiColor c);

/* Same thing for an arbitrary 24-bit color, e.g. ui_rgb(255, 0, 0) for red.
 * Values outside 0..255 get clamped. Safe to use up to 4 times in one printf. */
const char *ui_rgb(int r, int g, int b);

/* Shorthands so printf stays readable: printf("%sHi%s\n", C_BRAND, C_OFF); */
#define C_OFF     ui_c(UI_OFF)
#define C_BRAND   ui_c(UI_G3)
#define C_LIGHT   ui_c(UI_G1)
#define C_DARK    ui_c(UI_G5)
#define C_ACCENT  ui_c(UI_ACCENT)
#define C_INFO    ui_c(UI_INFO)
#define C_WARN    ui_c(UI_WARN)
#define C_ERR     ui_c(UI_ERR)
#define C_MUTED   ui_c(UI_MUTED)
#define C_DIM     ui_c(UI_DIM)
#define C_BOLD    ui_c(UI_BOLD)
#define C_TITLE   ui_c(UI_TITLE)

/* ------------------------------------------------------------------------------------
 * 2. ICONS (UTF-8)
 * ------------------------------------------------------------------------------------ */
#define ICON_OK      "✔"
#define ICON_FAIL    "✖"
#define ICON_WARN    "⚠"
#define ICON_INFO    "ℹ"
#define ICON_ARROW   "→"
#define ICON_BULLET  "●"
#define ICON_TODO    "○"
#define ICON_MOON    "☾"
#define ICON_PROMPT  "❯"
#define ICON_DOT     "·"

/* ------------------------------------------------------------------------------------
 * 3. STARTUP
 * ------------------------------------------------------------------------------------ */
void ui_init(void);       /* decide whether color is on (TTY / NO_COLOR / MOON_FORCE_COLOR) */
int  ui_has_color(void);

/* ------------------------------------------------------------------------------------
 * 4. BIG BLOCKS
 * ------------------------------------------------------------------------------------ */
void ui_logo(void);              /* gradient ASCII logo + tagline + version */
void ui_title_line(void);        /* the same brand on a single line (used by `help`) */
void ui_welcome(void);           /* logo + startup hints */
void ui_farewell(void);          /* goodbye line */
void ui_prompt(int last_status); /* the prompt:  moon ~/Projects/OS ❯ */

/* ------------------------------------------------------------------------------------
 * 5. SMALL PIECES
 * ------------------------------------------------------------------------------------ */
void ui_section(const char *title);   /*   ┌ title ────────────────  */
void ui_divider(void);                /*   ───────── ☾ ─────────     */
void ui_blank(void);                  /*   one empty line            */

/* Status messages. All of them take printf-style formats. */
void ui_ok(const char *fmt, ...)   __attribute__((format(printf, 1, 2)));
void ui_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_hint(const char *fmt, ...) __attribute__((format(printf, 1, 2)));  /* gray, indented */

/* Two aligned columns:  name   description  */
void ui_row(const char *name, const char *desc);

/* ------------------------------------------------------------------------------------
 * 6. BOXES   ╭─ Title ─────────────╮
 * ------------------------------------------------------------------------------------
 * Typical use:
 *     ui_box_top("Metadata");
 *     ui_box_kv("Inode", "%lu", st.st_ino);
 *     ui_box_bottom();
 */
void ui_box_top(const char *title);
void ui_box_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_box_kv(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void ui_box_sep(void);
void ui_box_bottom(void);

/* ------------------------------------------------------------------------------------
 * 7. SYSCALL TRACING  (the teaching part, strace-flavored)
 * ------------------------------------------------------------------------------------
 * Looks like this:
 *
 *   ┌ d_create ─────────────────────────────────────────
 *   │ open("hi.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644)    → 3
 *   │ write(3, "hello", 5)                              → 5
 *   │ close(3)                                          → 0
 *   └ ✔  File created (5 bytes)
 *
 * How a command uses it:
 *
 *     ui_trace_begin("d_create");
 *     ui_syscall("open(\"%s\", O_RDONLY)", path);   // opens the line
 *     int fd = open(path, O_RDONLY);
 *     if (fd == -1) { ui_ret_err(); ui_trace_fail("Could not open it"); return 1; }
 *     ui_ret_int(fd);                                // closes the line
 *     ...
 *     ui_trace_ok("Done");
 */
void ui_trace_begin(const char *cmd_name);
void ui_syscall(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_ret_int(long value);            /* → 3         (moonlight) */
void ui_ret_ptr(const void *p);         /* → 0x7f..    (moonlight) */
void ui_ret_err(void);                  /* → -1 ENOENT (No such file...)  (red, reads errno) */
void ui_trace_note(const char *fmt, ...) __attribute__((format(printf, 1, 2))); /* │  gray note */
void ui_trace_ok(const char *fmt, ...)   __attribute__((format(printf, 1, 2))); /* └ ✔ ... */
void ui_trace_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2))); /* └ ✖ ... */

/* Visible width of a UTF-8 string (skips ANSI escapes). Handy for alignment. */
size_t ui_width(const char *s);

#endif /* UI_H */
