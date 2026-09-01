#!/bin/sh
set -e

cd "$(dirname "$0")/.."
make >/dev/null

F=$(mktemp)
trap 'rm -f "$F" "$F.want"' EXIT

run() { printf '%s\nq\nexit\n' "$1" | ./moon >/dev/null 2>&1; }
check() {
    printf '%b' "$2" > "$F.want"
    if cmp -s "$F" "$F.want"; then
        echo "  ok   $1"
    else
        echo "  FAIL $1"
        echo "       want: $(tr '\n' '/' < "$F.want")"
        echo "       got:  $(tr '\n' '/' < "$F")"
        rm -f "$F.want"
        exit 1
    fi
    rm -f "$F.want"
}

rm -f "$F"
run "edit
o $F"
[ -f "$F" ] || { echo "  FAIL o creates the file"; exit 1; }
echo "  ok   o creates the file"

run "edit
o $F
a one
a two
a three"
check "a appends" 'one\ntwo\nthree\n'

run "edit
o $F
d 2"
check "d deletes the middle line" 'one\nthree\n'

run "edit
o $F
d 2"
check "d deletes the last line" 'one\n'

run "edit
o $F
i 1 zero"
check "i inserts at the top" 'zero\none\n'

run "edit
o $F
i 99 last"
check "i past the end appends" 'zero\none\nlast\n'

printf 'edit\no %s\np 2\nq\nexit\n' "$F" | ./moon 2>&1 \
    | sed 's/\x1b\[[0-9;]*m//g' | grep -qE '❯ one$' \
    && echo "  ok   p prints one line" \
    || { echo "  FAIL p prints one line"; exit 1; }

printf 'edit\no %s\ns last\nq\nexit\n' "$F" | ./moon 2>&1 \
    | sed 's/\x1b\[[0-9;]*m//g' | grep -qE '3 +last$' \
    && echo "  ok   s finds the line number" \
    || { echo "  FAIL s finds the line number"; exit 1; }

printf 'edit\no %s\ns zebra\nq\nexit\n' "$F" | ./moon 2>&1 | grep -q "not found" \
    && echo "  ok   s reports a miss" \
    || { echo "  FAIL s reports a miss"; exit 1; }

printf 'edit\no /nope/nothing.txt\nq\nexit\n' | ./moon 2>&1 | grep -q "No such file" \
    && echo "  ok   open failure calls perror" \
    || { echo "  FAIL open failure calls perror"; exit 1; }

printf 'edit\no %s\no /nope/x.txt\np 1\nq\nexit\n' "$F" | ./moon 2>&1 \
    | sed 's/\x1b\[[0-9;]*m//g' | grep -qE '❯ zero$' \
    && echo "  ok   a failed o keeps the current file" \
    || { echo "  FAIL a failed o keeps the current file"; exit 1; }

printf 'edit\np\nq\nexit\n' | ./moon 2>&1 | grep -q "no file open" \
    && echo "  ok   commands refuse without an open file" \
    || { echo "  FAIL commands refuse without an open file"; exit 1; }

printf 'no-newline' > "$F"
run "edit
o $F
a added"
check "a fixes a missing trailing newline" 'no-newline\nadded\n'

vis() { python3 tests/visual_driver.py "$F" "$1" >/dev/null 2>&1; }

printf 'alpha\nbeta\n' > "$F"
vis '\x1b[B|\x1b[C\x1b[C\x1b[C\x1b[C|!!|\x0f'
check "visual types at the cursor and ^O saves" 'alpha\nbeta!!\n'

printf 'alpha\nbeta\n' > "$F"
vis '\x1b[B|\x1b[C\x1b[C\x1b[C\x1b[C|\r|gamma|\x0f'
check "visual splits a line with Enter" 'alpha\nbeta\ngamma\n'

printf 'alpha\nbeta\n' > "$F"
vis '\x1b[B|\x1b[C\x1b[C\x1b[C\x1b[C|\x7f\x7f|\x0f'
check "visual backspace deletes characters" 'alpha\nbe\n'

printf 'alpha\nbeta\n' > "$F"
vis '\x1b[B|zzz'
check "visual without ^O leaves the file untouched" 'alpha\nbeta\n'

echo
echo "  all passed"
