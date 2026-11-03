#!/bin/sh
# Builds kernel/mmap_guard.ko, loads it on a test file and runs the attacks against it.
# Linux only. Needs sudo for insmod/rmmod/dmesg and the running kernel's build tree
# (override with KDIR=/path/to/kernel/build).
set -e

cd "$(dirname "$0")/.."
F=/tmp/moon_secret.txt
O=/tmp/moon_other.txt
B=$(mktemp -d)
SYS=/sys/module/mmap_guard/parameters

unload() { sudo rmmod mmap_guard 2>/dev/null || true; }
trap 'unload; rm -rf "$B"' EXIT
fail() { echo "  FAIL $1"; exit 1; }

make >/dev/null
make -C kernel ${KDIR:+KDIR=$KDIR} 2>&1 | grep -iE "warning|error" && fail "module builds without warnings"
echo "  ok   module builds without warnings"

CFLAGS="-Wall -Wextra -Werror -std=gnu99 -D_GNU_SOURCE -O2 -pthread"
${CC:-cc} $CFLAGS tests/mmap_attacks.c -o "$B/attacks"
${CC:-cc} $CFLAGS tests/mmap_bench.c -o "$B/bench"

unload
printf 'confidential: moon parcial 3\n' > "$F"
printf 'ordinary file\n' > "$O"
chmod 644 "$F" "$O"

echo
echo "without the module"
"$B/attacks" "$F" open
"$B/bench" "$F" "$O" > "$B/bench_off"

echo
echo "with the module"
sudo dmesg -C
sudo insmod kernel/mmap_guard.ko path="$F"
"$B/attacks" "$F" guarded

printf 'm_fmap %s w\nexit\n' "$F" | ./moon 2>&1 | grep -q "EACCES" \
    && echo "  ok   moon m_fmap w shows mmap -> EACCES" \
    || fail "moon m_fmap w shows mmap -> EACCES"
printf 'm_fmap %s r\nexit\n' "$F" | ./moon 2>&1 | grep -q "Mapped" \
    && echo "  ok   moon m_fmap r still maps the file" \
    || fail "moon m_fmap r still maps the file"

sudo dmesg | grep -q "mmap_guard: DENY mmap (shared-write)" \
    && echo "  ok   denial is in the kernel log" \
    || fail "denial is in the kernel log"
[ "$(cat $SYS/denied)" -gt 0 ] \
    && echo "  ok   counters: allowed=$(cat $SYS/allowed) denied=$(cat $SYS/denied)" \
    || fail "denied counter moved"

"$B/bench" "$F" "$O" > "$B/bench_on"
unload

"$B/attacks" "$F" open > /dev/null \
    && echo "  ok   after rmmod the shared write works again" \
    || fail "after rmmod the shared write works again"

echo
echo "overhead: mmap + first page fault + munmap, 200000 iterations"
echo " module off"; cat "$B/bench_off"
echo " module on";  cat "$B/bench_on"

echo
echo "  all passed"
