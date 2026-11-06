#!/bin/sh
# Step-by-step demo of mmap_guard: press Enter to move to the next step.
# Linux only, needs sudo. Run from the repository root:
#   sh kernel/demo.sh
# KDIR=/path/to/kernel/build and KCC=gcc-NN work as in tests/run_mmap_guard_tests.sh.
set -e

cd "$(dirname "$0")/.."
F=/tmp/moon_secret.txt
export MOON_FORCE_COLOR=1

step() { printf '\n\033[1;35m== %s\033[0m\n' "$1"; printf '   (Enter) '; read -r _; }
show() { printf '\033[2m$ %s\033[0m\n' "$*"; sh -c "$*" || true; }
moon() { printf '%s\nexit\n' "$1" | ./moon 2>&1 | sed -n '/┌/,/╰\|└/p'; }

sudo rmmod mmap_guard 2>/dev/null || true
trap 'sudo rmmod mmap_guard 2>/dev/null || true' EXIT

step "1. Build moon and the kernel module"
show "make -s"
show "make -s -C kernel ${KDIR:+KDIR=$KDIR} ${KCC:+CC=$KCC} 2>&1 | grep -iE 'warning|error'"
show "ls -l kernel/mmap_guard.ko"

step "2. A file this user owns and may write"
printf 'confidential: moon parcial 3\n' > "$F"
chmod 644 "$F"
show "ls -l $F; cat $F"

step "3. Without the module: a shared writable mapping changes the file"
moon "m_fmap $F w"
show "cat $F"
printf 'confidential: moon parcial 3\n' > "$F"

step "4. Load the module on that file"
show "sudo insmod kernel/mmap_guard.ko path=$F"
show "sudo dmesg | grep mmap_guard | tail -1"

step "5. Shared write: refused"
moon "m_fmap $F w"

step "6. Read only: allowed"
moon "m_fmap $F r"

step "7. Private copy (copy-on-write): allowed, the file stays the same"
moon "m_fmap $F p"
show "cat $F"

step "8. Execute, and widening with mprotect: refused"
moon "m_fmap $F x"
moon "m_fmap $F u"

step "9. Attack suite, including the Dirty COW race"
show "cc -Wall -O2 -pthread tests/mmap_attacks.c -o /tmp/mmap_attacks"
show "/tmp/mmap_attacks $F guarded"

step "10. Audit: kernel log and counters"
show "sudo dmesg | grep 'mmap_guard: DENY' | tail -5"
show "cat /sys/module/mmap_guard/parameters/allowed /sys/module/mmap_guard/parameters/denied"

step "11. Unload: the shared write works again"
show "sudo rmmod mmap_guard"
moon "m_fmap $F w"
printf '\ndone\n'
