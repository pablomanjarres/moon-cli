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

`d_create`, `p_fork` and `color` are implemented. The other 12 show up in `help` marked
`○` and are yours to write — each one tells you which file and which syscalls it needs.

`color` has its own page: [docs/color.md](docs/color.md).

MIT.
