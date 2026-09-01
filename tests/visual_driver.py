import fcntl, os, pty, struct, sys, termios, time

F = sys.argv[1]
KEYS = sys.argv[2].encode().decode("unicode_escape").encode("latin-1")

pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm"
    os.execv("./moon", ["./moon"])

fcntl.fcntl(fd, fcntl.F_SETFL, os.O_NONBLOCK)
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))


def drain(t=0.4):
    end = time.time() + t
    while time.time() < end:
        try:
            if not os.read(fd, 65536):
                break
        except (OSError, BlockingIOError):
            time.sleep(0.02)


drain(0.8)
os.write(fd, b"edit -v " + F.encode() + b"\n")
time.sleep(0.6)
drain(0.4)

for k in KEYS.split(b"|"):
    os.write(fd, k)
    time.sleep(0.25)
    drain(0.2)

os.write(fd, b"\x18")
time.sleep(0.4)
try:
    os.kill(pid, 9)
except OSError:
    pass
os.waitpid(pid, 0)
