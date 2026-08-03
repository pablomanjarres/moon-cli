# `color`

Prints text with one color per letter.

```
color <text> [-<color>-<index>]...
```

## Examples

```
color pablo                    rainbow, a different color per letter
color pablo -red-0             ...but letter 0 is forced to red
color pablo -pink-0 -gold-4    as many overrides as you want
color "hello world" -cyan-6    quote it if it has spaces
```

Letters count from **0**, so `-red-0` is the first letter and `-red-4` is the fifth.

## Flags

A flag is `-` + a color name + `-` + a letter number. Nothing else.

Without flags every letter follows the rainbow cycle, which repeats every 7 letters.
A flag replaces the color of exactly one letter. If two flags point at the same
letter, the last one wins.

> Heads up: the rainbow already starts on red, so `-red-0` looks like it does
> nothing. Try `-pink-0` or `-moon-0` to see the override clearly.

## Colors

| name | rgb | | name | rgb |
|---|---|---|---|---|
| `red` | 255, 0, 0 | | `pink` | 255, 105, 180 |
| `orange` | 255, 127, 0 | | `white` | 255, 255, 255 |
| `yellow` | 255, 255, 0 | | `gray` | 139, 147, 167 |
| `green` | 0, 255, 0 | | `gold` | 233, 196, 106 |
| `cyan` | 0, 255, 255 | | `moon` | 180, 196, 228 |
| `blue` | 0, 127, 255 | | | |
| `indigo` | 75, 0, 130 | | | |
| `violet` | 148, 0, 211 | | | |

To add one, drop a row into the `NAMED` table at the top of `cat_util.c`. It works
immediately — the parser and the error messages read that same table.

## Errors

| you typed | what you get |
|---|---|
| `color` | wrong usage, plus the color list |
| `color pablo -mauve-0` | `I do not know that color` |
| `color pablo -red` | `write it as -<color>-<number>` |
| `color pablo red-0` | `a flag has to start with '-'` |
| `color pablo -red-0x` | `that is not a valid letter number` |
| `color pablo -red-99` | `points at letter 99, but "pablo" only has 5` |

## Limits

- Text longer than **256** characters is refused. Raise `MAX_LETTERS` in
  `cat_util.c` if you need more.
- Indexes count **bytes, not characters**. `pablo` is fine; `mañana` has a 2-byte
  `ñ`, so the letters after it shift and the `ñ` itself gets split across two colors.
- Color turns itself off when the output is not a terminal, so
  `./moon > out.txt` writes plain text. `MOON_FORCE_COLOR=1` overrides that.

## How it works

Three steps, all in `cmd_color()`:

1. Fill an array with the rainbow color for every letter.
2. Walk the flags and overwrite individual entries in that array.
3. Print each letter preceded by `ui_rgb(r, g, b)`, then reset with `C_OFF`.

`ui_rgb()` lives in `ui.c` and just builds the escape sequence
`\033[38;2;R;G;Bm`. The terminal reads those bytes as "switch to this color"
instead of printing them — which is why the whole thing is really just one
`write(2)` to file descriptor 1.
