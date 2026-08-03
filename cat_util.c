/**
 * ====================================================================================
 * cat_util.c — CATEGORY "util": short, handy system commands
 * ====================================================================================
 * Syscalls in this family: getuid(2), time(2), write(2).
 *
 * IMPLEMENTED HERE:
 *   color   paints text, one color per letter
 *
 * PENDING:
 *   greet   getuid() + getpwuid() to find out who you are
 *   time    time() + localtime(), formatted as HH:MM:SS
 *   date    time() + localtime(), formatted as YYYY-MM-DD
 *
 * These are the easiest commands in the shell, so start here if you want to get the
 * rhythm before touching memory or processes.
 *
 * The recipe:
 *   1. write the function here
 *   2. uncomment its prototype in shell.h
 *   3. in registry.c, swap that row's NULL for the function name
 */

#include "shell.h"

#include <limits.h>   /* INT_MAX, for bounds-checking the flag index */

/* Uncomment what you need once you start:
 *
 * #include <unistd.h>   // getuid
 * #include <pwd.h>      // getpwuid
 * #include <time.h>     // time, localtime, strftime
 */

/* ====================================================================================
 * color <text> [-<color>-<index>]...
 * ====================================================================================
 * Prints the text with one color per letter.
 *
 *   color pablo                  every letter a different color (rainbow, cycling)
 *   color pablo -red-0           ...except letter 0, forced to red
 *   color pablo -red-0 -gold-4   as many overrides as you want
 *
 * Indexes start at 0, so -red-0 is the first letter.
 *
 * Underneath this is just write(2): printf pushes bytes into file descriptor 1
 * (stdout), and some of those bytes are ANSI escape sequences that the terminal
 * reads as "switch to this color" instead of printing them.
 */

#define MAX_LETTERS 256   /* longest text this command will paint */

/* The colors you can name in a flag. Add a row and it works immediately. */
typedef struct {
    const char *name;
    int r, g, b;
} NamedColor;

static const NamedColor NAMED[] = {
    { "red",    255,   0,   0 },
    { "orange", 255, 127,   0 },
    { "yellow", 255, 255,   0 },
    { "green",    0, 255,   0 },
    { "cyan",     0, 255, 255 },
    { "blue",     0, 127, 255 },
    { "indigo",  75,   0, 130 },
    { "violet", 148,   0, 211 },
    { "pink",   255, 105, 180 },
    { "white",  255, 255, 255 },
    { "gray",   139, 147, 167 },
    { "gold",   233, 196, 106 },
    { "moon",   180, 196, 228 },
};
static const int NUM_NAMED = (int)(sizeof(NAMED) / sizeof(NAMED[0]));

/* The default cycle, used for every letter you do not override. */
static const int RAINBOW[7][3] = {
    {255,   0,   0}, {255, 127,   0}, {255, 255,   0}, {  0, 255,   0},
    {  0, 127, 255}, { 75,   0, 130}, {148,   0, 211},
};

/* Prints the list of color names, for when someone gets one wrong. */
static void list_colors(void)
{
    char line[512];
    size_t used = 0;

    line[0] = '\0';
    for (int i = 0; i < NUM_NAMED; i++) {
        int n = snprintf(line + used, sizeof(line) - used, "%s%s",
                         NAMED[i].name, (i < NUM_NAMED - 1) ? " " : "");
        if (n < 0 || (size_t)n >= sizeof(line) - used) break;
        used += (size_t)n;
    }
    ui_hint("colors: %s%s%s", C_ACCENT, line, C_OFF);
}

/**
 * Takes one flag like "-red-0" apart.
 *
 * Returns 1 when it parsed, 0 when the shape is wrong (and then *why says what for).
 * On success *index gets the letter position and *color points at the palette row.
 */
static int parse_flag(const char *flag, int *index, const NamedColor **color,
                      const char **why)
{
    if (flag[0] != '-') {
        *why = "a flag has to start with '-'";
        return 0;
    }

    const char *body = flag + 1;              /* "red-0"                  */
    const char *dash = strrchr(body, '-');    /* the LAST dash splits it  */
    if (dash == NULL || dash == body || dash[1] == '\0') {
        *why = "write it as -<color>-<number>, for example -red-0";
        return 0;
    }

    /* Whatever follows the last dash has to be a plain, sane number.
     *
     * The INT_MAX check is not paranoia: strtol hands back a long, and casting a
     * long that big to int wraps around to a NEGATIVE number. That negative index
     * would sail past the "is it past the end of the text?" test below and write
     * outside the array. Catch it here instead. */
    char *end;
    long n = strtol(dash + 1, &end, 10);
    if (*end != '\0' || n < 0 || n > INT_MAX) {
        *why = "that is not a valid letter number";
        return 0;
    }

    /* and whatever precedes it has to be a color we know */
    size_t name_len = (size_t)(dash - body);
    for (int i = 0; i < NUM_NAMED; i++) {
        if (strlen(NAMED[i].name) == name_len &&
            strncmp(body, NAMED[i].name, name_len) == 0) {
            *index = (int)n;
            *color = &NAMED[i];
            return 1;
        }
    }

    *why = "I do not know that color";
    return 0;
}

int cmd_color(int argc, char **argv)
{
    if (argc < 2) {
        registry_usage("color");
        ui_hint("example: %scolor pablo -red-0%s", C_ACCENT, C_OFF);
        list_colors();
        return 1;
    }

    const char *text = argv[1];
    int len = (int)strlen(text);

    if (len > MAX_LETTERS) {
        ui_fail("That text is too long (%d letters, the limit is %d).",
                len, MAX_LETTERS);
        return 1;
    }

    /* --- 1. start every letter on the rainbow cycle ------------------------------- */
    int rgb[MAX_LETTERS][3];
    for (int i = 0; i < len; i++) {
        rgb[i][0] = RAINBOW[i % 7][0];
        rgb[i][1] = RAINBOW[i % 7][1];
        rgb[i][2] = RAINBOW[i % 7][2];
    }

    /* --- 2. let the flags overwrite individual letters ---------------------------- */
    for (int a = 2; a < argc; a++) {
        int index = 0;
        const NamedColor *c = NULL;
        const char *why = "";

        if (!parse_flag(argv[a], &index, &c, &why)) {
            ui_fail("Bad flag '%s': %s.", argv[a], why);
            ui_hint("usage: %scolor <text> [-<color>-<index>]%s", C_ACCENT, C_OFF);
            list_colors();
            return 1;
        }
        if (index >= len) {
            ui_fail("'%s' points at letter %d, but \"%s\" only has %d.",
                    argv[a], index, text, len);
            if (len > 0)   /* empty text has no "last letter" to point at */
                ui_hint("letters count from 0, so the last one here is %d.", len - 1);
            return 1;
        }

        rgb[index][0] = c->r;
        rgb[index][1] = c->g;
        rgb[index][2] = c->b;
    }

    /* --- 3. print it, one color per letter ---------------------------------------- */
    printf("  ");
    for (int i = 0; i < len; i++)
        printf("%s%c", ui_rgb(rgb[i][0], rgb[i][1], rgb[i][2]), text[i]);
    printf("%s\n", C_OFF);

    return 0;
}

/**
 * ------------------------------------------------------------------------------------
 * TEMPLATE: time
 * ------------------------------------------------------------------------------------
 * time() returns the number of seconds since January 1st 1970 (the "Unix epoch").
 * localtime() turns that huge number into day, month, hour and minute according to
 * the system's timezone.
 *
 * int cmd_time(int argc, char **argv)
 * {
 *     (void)argc; (void)argv;
 *
 *     ui_trace_begin("time");
 *
 *     ui_syscall("time(NULL)");
 *     time_t now = time(NULL);
 *     if (now == (time_t)-1) { ui_ret_err(); ui_trace_fail("time failed."); return 1; }
 *     ui_ret_int((long)now);
 *
 *     char buf[64];
 *     struct tm *t = localtime(&now);
 *     strftime(buf, sizeof(buf), "%H:%M:%S", t);
 *
 *     ui_trace_ok("It is %s.", buf);
 *     return 0;
 * }
 */
