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
import subprocess
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

    def command(self, text, width=80, exits=False):
        # Each printable key causes a frame; do not accept a partial typed command.
        typed = ""
        for key in text:
            typed = typed[:-1] if key == "\x7f" else typed + key
            frame = check_layout(self.visual(key), width)
            assert typed[-min(16, width // 2):] in frame, "typed command is hidden"
        if exits:
            return self.send("\r", SHELL)
        return check_layout(self.visual("\r"), width)

    def raw(self):
        flags = termios.tcgetattr(self.fd)[3]
        assert not flags & (termios.ICANON | termios.ECHO), "command bar left raw mode"

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


def check_view(text, file, lines):
    rows = text.split("\r\n")
    assert file.name in rows[0], "file name disappeared from view"
    for number, line in enumerate(lines, 1):
        pattern = r"\s*" + str(number) + r"[ \t|│:]+" + re.escape(line) + r"\s*"
        assert re.fullmatch(pattern, rows[number]), "numbered document line disappeared: " + line
    assert not re.search(r"\bfd\s+\d+", text), "raw descriptor leaked into UI"


def commands_from_visual(folder, width):
    file = folder / "commands.txt"
    other = folder / "other.txt"
    file.write_bytes(b"alpha\nbeta\n")
    other.write_bytes(b"other\n")
    session = Session(width)
    try:
        frame = session.visual(f"edit {file}\n")
        assert not re.search(r"\bfd\s+\d+", ANSI.sub("", frame)), "open leaked raw descriptor"
        check_help(frame, width)
        text = check_layout(session.visual("\x0c"), width)
        session.raw()
        check_view(text, file, ("alpha", "beta"))
        printed = session.command("p", width)
        check_view(printed, file, ("alpha", "beta"))
        results = "\r\n".join(printed.split("\r\n")[3:])
        assert re.search(r"\b1\b[^\r\n]*alpha", results), "p lost first result line"
        assert re.search(r"\b2\b[^\r\n]*beta", results), "p lost second result line"
        printed = session.command("p 2", width)
        check_view(printed, file, ("alpha", "beta"))
        results = "\r\n".join(printed.split("\r\n")[3:])
        assert re.search(r"\b2\b[^\r\n]*beta", results), "p 2 lost selected result"
        text = session.command("a typo\x7f\x7f\x7f\x7fadded", width)
        assert file.read_bytes() == b"alpha\nbeta\nadded\n"
        check_view(text, file, ("alpha", "beta", "added"))
        text = session.command("i 2 inserted", width)
        assert file.read_bytes() == b"alpha\ninserted\nbeta\nadded\n"
        check_view(text, file, ("alpha", "inserted", "beta", "added"))
        found = session.command("s inserted", width)
        results = "\r\n".join(found.split("\r\n")[5:])
        assert re.search(r"\b2\b[^\r\n]*inserted", results), "s lost line number"
        assert re.search(r"not found|no matches|no results", session.command("s absent", width), re.I)
        text = session.command("d 1", width)
        assert file.read_bytes() == b"inserted\nbeta\nadded\n"
        check_view(text, file, ("inserted", "beta", "added"))
        text = session.command(f"o {other}", width)
        check_view(text, other, ("other",))
        text = session.command("o /nope/missing.txt", width)
        check_view(text, other, ("other",))
        assert re.search(r"no such|cannot|failed|error", text, re.I), "open error missing"
        session.command("a changed", width)
        assert other.read_bytes() == b"other\nchanged\n"
        assert file.read_bytes() == b"inserted\nbeta\nadded\n"
        session.command("q", exits=True)
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
        session.raw()
        assert file.read_bytes() == b"alpha\nbeta\n", "unsaved content reached disk"
        for command in ("a blocked", "d 1", "i 1 blocked", "o /nope/blocked.txt"):
            text = session.command(command)
            check_view(text, file, ("Xalpha", "beta"))
            assert re.search(r"save.*\^O|\^O.*save", text, re.I), "missing save-first hint"
            assert file.read_bytes() == b"alpha\nbeta\n", "dirty command changed disk"
        session.visual("\x0f")
        assert file.read_bytes() == b"Xalpha\nbeta\n"
        text = session.command("a added")
        check_view(text, file, ("Xalpha", "beta", "added"))
        assert file.read_bytes() == b"Xalpha\nbeta\nadded\n"
        session.visual("\x0c")
        text = check_layout(session.visual("Y"), 80)
        body = text.split("\r\n")[1:4]
        assert any("Y" in row for row in body), "Ctrl+L did not return document focus"
        session.send("\x18", SHELL)
        session.restored()
        assert file.read_bytes() == b"Xalpha\nbeta\nadded\n", "^X saved without request"
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


def legacy_visual_return(folder):
    file = folder / "legacy.txt"
    file.write_bytes(b"alpha\nbeta\n")
    session = Session(80)
    try:
        session.send("edit\n")
        session.send(f"o {file}\n")
        session.visual("v\n")
        session.send("\x18")
        session.restored()
        assert "\nbeta\r\n" in ANSI.sub("", session.send("p 2\n"))
        session.send("q\n", SHELL)
        session.restored()
    finally:
        session.close()


def legacy_long_match(folder):
    file = folder / "long-match.txt"
    line = "needle " + "x" * 700 + " tail"
    file.write_text(line + "\n")
    result = subprocess.run(["./moon"], input=f"edit\no {file}\ns needle\nq\nexit\n",
                            text=True, capture_output=True, timeout=3, check=True)
    assert line in ANSI.sub("", result.stdout), "legacy search truncated matched text"
    assert file.read_text() == line + "\n", "search modified its file"


def cancel_command(folder):
    file = folder / "cancel.txt"
    file.write_bytes(b"alpha\nbeta\n")
    session = Session(80)
    try:
        session.visual(f"edit {file}\n")
        session.visual("\x0c")
        for key in "a discarded":
            session.visual(key)
        text = check_layout(session.visual("\x1b"), 80)
        check_view(text, file, ("alpha", "beta"))
        assert file.read_bytes() == b"alpha\nbeta\n", "ESC executed pending command"
        frame = session.send("Z", r"Zalpha[\s\S]*\x1b\[\?25h$", raw=True)
        text = check_layout(frame, 80)
        check_view(text, file, ("Zalpha", "beta"))
        assert file.read_bytes() == b"alpha\nbeta\n", "ESC saved visual edits"
        session.visual("\x0c")
        session.visual("\x0f")
        session.command("q", exits=True)
        session.restored()
    finally:
        session.close()


def dirty_quit(folder):
    file = folder / "dirty-quit.txt"
    file.write_bytes(b"alpha\nbeta\n")
    session = Session(80)
    try:
        session.visual(f"edit {file}\n")
        session.visual("X")
        session.visual("\x0c")
        text = session.command("q")
        check_view(text, file, ("Xalpha", "beta"))
        session.raw()
        assert re.search(r"save.*\^O|\^O.*save", text, re.I), "dirty q omitted save hint"
        assert file.read_bytes() == b"alpha\nbeta\n", "dirty q changed disk"
        session.visual("\x0f")
        assert file.read_bytes() == b"Xalpha\nbeta\n"
        session.command("q", exits=True)
        session.restored()
    finally:
        session.close()


def delayed_arrow(folder, prefix_delay, direction_delay):
    file = folder / "arrow.txt"
    file.write_bytes(b"alpha\nbeta\n")
    session = Session(80)
    try:
        session.visual(f"edit {file}\n")
        os.write(session.fd, b"\x1b" if prefix_delay else b"\x1b[")
        if prefix_delay:
            time.sleep(prefix_delay)
            os.write(session.fd, b"[")
        time.sleep(direction_delay)
        os.write(session.fd, b"B")
        # A distinct bar frame is a barrier after all fragmented input bytes.
        frame = session.send("\x0c", r"Command: [^\r\n]*\x1b\[\?25h$", raw=True)
        check_view(check_layout(frame, 80), file, ("alpha", "beta"))
        session.raw()
        session.visual("\x0c")
        text = check_layout(session.visual("Z"), 80)
        check_view(text, file, ("alpha", "Zbeta"))
        assert file.read_bytes() == b"alpha\nbeta\n", "fragmented arrow saved text"
        session.send("\x18", SHELL)
        session.restored()
    finally:
        session.close()


def queued_escape(folder):
    file = folder / "queued-escape.txt"
    file.write_bytes(b"alpha\nbeta\n")
    session = Session(80)
    try:
        session.visual(f"edit {file}\n")
        session.visual("\x0c")
        session.visual("a")
        os.write(session.fd, b"\x1bZ")
        frame = session.send("\x0c", r"Command: [^\r\n]*\x1b\[\?25h$", raw=True)
        check_view(check_layout(frame, 80), file, ("Zalpha", "beta"))
        assert file.read_bytes() == b"alpha\nbeta\n", "queued Escape saved text"
        session.visual("\x0f")
        session.command("q", exits=True)
        session.restored()
    finally:
        session.close()


def literal_after_escape(folder, literal):
    file = folder / "literal.txt"
    file.write_bytes(b"alpha\nbeta\n")
    session = Session(80)
    try:
        session.visual(f"edit {file}\n")
        session.visual("\x0c")
        session.visual("a")
        session.visual("\x1b")
        for key in literal:
            session.visual(key)
        frame = session.send("\x0f", r"saved file[\s\S]*\x1b\[\?25h$", raw=True)
        wanted = (literal + "alpha\nbeta\n").encode()
        assert file.read_bytes() == wanted, "Escape consumed literal text: " + repr(file.read_bytes())
        check_view(check_layout(frame, 80), file, (literal + "alpha", "beta"))
        session.send("\x18", SHELL)
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
        check_layout(session.visual("\x0c"), 20)
        session.raw()
        session.command("a " + "y" * 60, 20)
        assert file.read_bytes() == b"x" * 25 + b"Z" + b"x" * 135 + b"\n" + b"y" * 60 + b"\n"
        session.command("q", width=20, exits=True)
        session.restored()
    finally:
        session.close()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="moon-command-mode-") as directory:
        folder = Path(directory)
        legacy_long_match(folder)
        cancel_command(folder)
        dirty_quit(folder)
        queued_escape(folder)
        for literal in ("[text]", "[123text]"):
            literal_after_escape(folder, literal)
        for prefix, direction in ((0.06, 0), (0.2, 0), (0, 0.06), (0, 0.2), (0.2, 0.2), (0.35, 0)):
            delayed_arrow(folder, prefix, direction)
        for width in (80, 50):
            commands_from_visual(folder, width)
        dirty_and_return(folder)
        help_without_file()
        legacy_visual_return(folder)
        narrow_and_long_line(folder)
    print("  ok   fullscreen commands, help, narrow UI, dirty protection and terminal restore")
