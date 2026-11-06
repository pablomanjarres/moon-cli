#!/bin/sh
# Overhead of mmap_guard on mmap() and on page-fault handling, with and without the
# module, measured with ftrace (function_graph) and strace -c.
# Linux only, needs sudo, strace and a built kernel/mmap_guard.ko. From the repo root:
#   sh tests/mmap_overhead.sh            (N=20000 iterations by default)
set -e

cd "$(dirname "$0")/.."
F=/tmp/moon_secret.txt
O=/tmp/moon_other.txt
T=/sys/kernel/tracing
N=${N:-20000}
B=$(mktemp -d)

reset_trace() { sudo sh -c "echo 0 > $T/tracing_on; echo nop > $T/current_tracer; echo > $T/set_ftrace_filter; echo 0 > $T/options/funcgraph-proc"; }
trap 'reset_trace; sudo rmmod mmap_guard 2>/dev/null || true; rm -rf "$B"' EXIT

cc -O2 -Wall -Wextra tests/mmap_bench.c -o "$B/bench"
printf 'confidential: moon parcial 3\n' > "$F"
printf 'ordinary file\n' > "$O"
sudo rmmod mmap_guard 2>/dev/null || true

# Average kernel time of vm_mmap_pgoff (one mmap) and handle_mm_fault (one page fault)
# for the benchmark process only: funcgraph-proc prints the process name on every line.
ftrace_avg() {
    reset_trace
    sudo sh -c "echo > $T/trace; echo 16384 > $T/buffer_size_kb
                echo 'vm_mmap_pgoff handle_mm_fault' > $T/set_ftrace_filter
                echo 1 > $T/options/funcgraph-proc
                echo function_graph > $T/current_tracer; echo 1 > $T/tracing_on"
    "$B/bench" "$F" "$O" "$N" > /dev/null
    sudo sh -c "echo 0 > $T/tracing_on"
    sudo cat $T/trace | awk '
        / bench-[0-9]+ / && /(vm_mmap_pgoff|handle_mm_fault)\(\);/ {
            if (!match($0, /[0-9]+\.[0-9]+ us/)) next
            f = ($0 ~ /vm_mmap_pgoff/) ? "mmap" : "fault"
            sum[f] += substr($0, RSTART, RLENGTH - 3); n[f]++
        }
        END {
            printf "    vm_mmap_pgoff    %6d calls  %6.0f ns avg\n", n["mmap"],  sum["mmap"]  / n["mmap"]  * 1000
            printf "    handle_mm_fault  %6d calls  %6.0f ns avg\n", n["fault"], sum["fault"] / n["fault"] * 1000
        }'
}

# mmap/munmap rows of strace -c: calls and microseconds per call.
strace_rows() {
    strace -c -e trace=mmap,munmap "$B/bench" "$F" "$O" "$N" 2>&1 >/dev/null \
        | awk '$NF == "mmap" || $NF == "munmap" { printf "    %-7s %7s calls  %5s us/call\n", $NF, $4, $3 }'
}

run() {
    echo "  ftrace (function_graph)"
    ftrace_avg
    echo "  strace -c"
    strace_rows
}

echo "module off, $N iterations per file"
run

sudo insmod kernel/mmap_guard.ko path="$F"
echo
echo "module on, $N iterations per file"
run
sudo rmmod mmap_guard

echo
echo "interception seen by strace (module on)"
sudo insmod kernel/mmap_guard.ko path="$F"
printf 'm_fmap %s w\nexit\n' "$F" > "$B/cmds"
strace -e trace=mmap,mprotect ./moon < "$B/cmds" 2>&1 >/dev/null | grep -E "PROT_WRITE.*MAP_SHARED, 3" || true
