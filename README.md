<div align="center">

```
███╗   ███╗ ██████╗  ██████╗ ███╗   ██╗
████╗ ████║██╔═══██╗██╔═══██╗████╗  ██║
██╔████╔██║██║   ██║██║   ██║██╔██╗ ██║
██║╚██╔╝██║██║   ██║██║   ██║██║╚██╗██║
██║ ╚═╝ ██║╚██████╔╝╚██████╔╝██║ ╚████║
╚═╝     ╚═╝ ╚═════╝  ╚═════╝ ╚═╝  ╚═══╝
```

**a shell that shows you the system calls behind every command**

</div>

## Run

```bash
git clone https://github.com/pablomanjarres/moon-cli.git
cd moon-cli
make
./moon
```

Needs Linux, `gcc` and `make`. Nothing else.

## Use

```
moon ~/moon-cli ❯ help                     list every command
moon ~/moon-cli ❯ d_create hi.txt "hey"    create a file
moon ~/moon-cli ❯ p_fork                   spawn a child process
moon ~/moon-cli ❯ color pablo --red=0      one color per letter
moon ~/moon-cli ❯ exit                     quit (Ctrl+D also works)
```

Every command traces the syscalls it makes:

```
  ┌ d_create ─────────────────────────────────────────────
  │ open("hi.txt", O_WRONLY|O_CREAT|O_TRUNC, 0644)       → 3
  │ write(3, "hey", 3)                                   → 3
  │ close(3)                                             → 0
  └ ✔  'hi.txt' created with 3 bytes.
```

`d_create`, `p_fork`, `color` and `m_fmap` are implemented. The other 12 show up in `help` marked
`○` and are yours to write — each one tells you which file and which syscalls it needs.

`color` has its own page: [docs/color.md](docs/color.md).
The text editor's design and commands are in [docs/editor.md](docs/editor.md).
The technical report is available as [docs/editor.pdf](docs/editor.pdf).
The concurrent Huffman project is in [docs/huffman.md](docs/huffman.md),
with its technical report in [docs/huffman.pdf](docs/huffman.pdf).
The mmap security project (kernel module `kernel/mmap_guard.c` and the `m_fmap`
command) is in [docs/mmap_guard.md](docs/mmap_guard.md), with its report in
[docs/mmap_guard.pdf](docs/mmap_guard.pdf).

## Kernel mmap guard

`kernel/mmap_guard.ko` refuses writable shared and executable mappings of one file,
including upgrades through `mprotect()`. Linux only. Needs `sudo` and the build tree
of the running kernel (`/lib/modules/$(uname -r)/build`, or pass `KDIR=`).

```bash
make && make -C kernel
echo "confidential" > /tmp/moon_secret.txt
sudo insmod kernel/mmap_guard.ko path=/tmp/moon_secret.txt
./moon                      # m_fmap /tmp/moon_secret.txt w  ->  mmap ... = -1 EACCES
sudo rmmod mmap_guard
```

| Script | What it does |
|---|---|
| `sh kernel/demo.sh` | Step-by-step demo; press Enter to advance |
| `sh tests/run_mmap_guard_tests.sh` | Attacks with and without the module, kernel log and counters |
| `sh tests/mmap_overhead.sh` | Overhead on `mmap()` and page faults, measured with ftrace and strace |

If the kernel was built with a compiler other than the default `gcc`, pass
`CC=gcc-NN` to `make -C kernel` and `KCC=gcc-NN` to the scripts.

MIT.
