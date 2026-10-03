"""Exercise the real editor, rendered command help, and terminal restoration."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import tempfile
import termios
import time

ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
PROMPT = r"ed ❯ $"
SHELL = r"moon [^\r\n]* ❯ $"


class Session:
    def __init__(self, width):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["TERM"] = "xterm"
            os.execv("./moon", ["./moon"])
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 24, width, 0, 0))
        try:
            self.expect(SHELL)
            self.saved = termios.tcgetattr(self.fd)
        except BaseException:
            self.close()
            raise

    def expect(self, pattern, raw=False):
        data = b""
        end = time.monotonic() + 3
        while time.monotonic() < end:
            if not select.select([self.fd], [], [], 0.05)[0]:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                break
            if not chunk:
                break
            data += chunk
            assert len(data) <= 1024 * 1024, "terminal output exceeded test limit"
            text = data.decode("utf-8", "replace")
            if re.search(pattern, text if raw else ANSI.sub("", text)):
                return text
        raise AssertionError(f"did not see {pattern!r}: {data[-1000:]!r}")

    def send(self, text, pattern=PROMPT, raw=False):
        os.write(self.fd, text.encode())
        return self.expect(pattern, raw)

    def visual(self, text):
        return self.send(text, r"\x1b\[\?25h$", raw=True)

    def restored(self):
        assert termios.tcgetattr(self.fd) == self.saved, "terminal not restored"

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(self.pid, 0)
        os.close(self.fd)


def check_layout(frame, width):
    frame = frame.rsplit("\x1b[2J", 1)[-1]
    text = ANSI.sub("", frame)
    assert len(text.split("\r\n")) <= 24, "help renders below terminal bottom"
    assert all(len(row) <= width for row in text.split("\r\n")), "UI wraps past width"
    for row, col in re.findall(r"\x1b\[(\d+);(\d+)H", frame):
        assert 1 <= int(row) <= 24 and 1 <= int(col) <= width, "cursor outside screen"
    return text


def check_help(frame, width):
    text = check_layout(frame, width)
    for syntax in (r"o <?file>?", r"p \[n\]", r"a <?text>?", r"d <?n>?",
                   r"i <?n>? <?text>?", r"s <?word>?"):
        assert re.search(r"\b" + syntax, text), f"help missing {syntax} at {width} columns"
    assert re.search(r"\bq\b", text), "fullscreen help missing q"
    assert re.search(r"\^L\s+commands", text), "fullscreen help missing ^L shortcut"


def commands_from_visual(folder, width):
    file = folder / "commands.txt"
    other = folder / "other.txt"
    file.write_bytes(b"alpha\nbeta\n")
    other.write_bytes(b"other\n")
    session = Session(width)
    try:
        frame = session.visual(f"edit {file}\n")
        # Try the shortcut first so the regression fails on unreachable commands.
        session.send("\x0c")
        session.restored()
        check_help(frame, width)
        printed = ANSI.sub("", session.send("p\n"))
        assert "\nalpha\r\nbeta\r\n" in printed, "p did not print the open file"
        printed = ANSI.sub("", session.send("p 2\n"))
        assert "\nbeta\r\n" in printed and "\nalpha\r\n" not in printed
        session.send("a added\n")
        assert file.read_bytes() == b"alpha\nbeta\nadded\n"
        session.send("i 2 inserted\n")
        assert file.read_bytes() == b"alpha\ninserted\nbeta\nadded\n"
        found = ANSI.sub("", session.send("s inserted\n"))
        assert re.search(r"\n\s*2\s+inserted\r\n", found), "s lost line number"
        session.send("d 1\n")
        assert file.read_bytes() == b"inserted\nbeta\nadded\n"
        session.send(f"o {other}\n")
        assert "\nother\r\n" in ANSI.sub("", session.send("p\n"))
        session.send("a changed\n")
        assert other.read_bytes() == b"other\nchanged\n"
        assert file.read_bytes() == b"inserted\nbeta\nadded\n"
        session.send("q\n", SHELL)
        session.restored()
    finally:
        session.close()


def dirty_and_return(folder):
    file = folder / "dirty.txt"
    file.write_bytes(b"alpha\nbeta\n")
    session = Session(80)
    try:
        session.visual(f"edit {file}\n")
        session.visual("X")
        frame = session.visual("\x0c")
        text = ANSI.sub("", frame)
        assert "Xalpha" in text, "dirty document was discarded"
        assert re.search(r"save.*\^O|\^O.*save", text, re.I), "missing save-first hint"
        assert not termios.tcgetattr(session.fd)[3] & termios.ICANON
        assert file.read_bytes() == b"alpha\nbeta\n", "unsaved content reached disk"
        session.visual("\x0f")
        assert file.read_bytes() == b"Xalpha\nbeta\n"
        session.send("\x0c")
        session.restored()
        assert "\nXalpha\r\nbeta\r\n" in ANSI.sub("", session.send("p\n"))
        frame = session.visual("v\n")
        assert "Xalpha" in frame, "v did not reopen the same file"
        session.send("\x18")
        session.restored()
        session.send("q\n", SHELL)
        session.restored()
        session.visual(f"edit {file}\n")
        session.visual("Y")
        session.send("\x18", SHELL)
        session.restored()
        assert file.read_bytes() == b"Xalpha\nbeta\n", "^X saved without request"
    finally:
        session.close()


def help_without_file():
    session = Session(80)
    try:
        for command in ("edit\n", "help\n", "?\n"):
            text = ANSI.sub("", session.send(command))
            assert "editor commands:" in text, "line mode did not show help"
            for name in ("o", "p", "a", "d", "i", "s", "q", "v"):
                assert re.search(r"\n\s*" + name + r"\s", text), "help omitted " + name
            assert "no file open" not in text, "help requires an open file"
        assert "no file open" in ANSI.sub("", session.send("p\n"))
        session.send("q\n", SHELL)
        session.restored()
    finally:
        session.close()


def narrow_and_long_line(folder):
    file = folder / "long.txt"
    file.write_bytes(b"x" * 160 + b"\n")
    session = Session(20)
    try:
        text = check_layout(session.visual(f"edit {file}\n"), 20)
        assert "o p a d i s q" in text, "narrow help omitted command names"
        assert "^L commands" in text, "narrow help omitted command shortcut"
        for _ in range(25):
            check_layout(session.visual("\x1b[C"), 20)
        check_layout(session.visual("Z"), 20)
        session.visual("\x0f")
        assert file.read_bytes() == b"x" * 25 + b"Z" + b"x" * 135 + b"\n"
        session.send("\x0c")
        session.restored()
        session.send("q\n", SHELL)
    finally:
        session.close()


with tempfile.TemporaryDirectory(prefix="moon-command-mode-") as directory:
    folder = Path(directory)
    for width in (80, 50):
        commands_from_visual(folder, width)
    dirty_and_return(folder)
    help_without_file()
    narrow_and_long_line(folder)
print("  ok   fullscreen commands, help, narrow UI, dirty protection and terminal restore")
