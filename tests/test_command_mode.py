"""Exercise the real editor, rendered command help, and terminal restoration."""
import errno
from contextlib import contextmanager
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import shlex
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
    def __init__(self, width, executable="./moon", env=None):
        self.transcript = ""
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["TERM"] = "xterm"
            if env:
                os.environ.update(env)
            os.execv(str(executable), [str(executable)])
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 24, width, 0, 0))
        try:
            self.expect(SHELL)
            self.saved = termios.tcgetattr(self.fd)
        except BaseException:
            self.close()
            raise

    def expect(self, pattern, raw=False, timeout=3):
        data = b""
        end = time.monotonic() + timeout
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
            self.record(chunk)
            assert len(data) <= 1024 * 1024, "terminal output exceeded test limit"
            text = data.decode("utf-8", "replace")
            if re.search(pattern, text if raw else ANSI.sub("", text)):
                return text
        raise AssertionError(f"did not see {pattern!r}: {data[-1000:]!r}")

    def record(self, chunk):
        self.transcript = (self.transcript + chunk.decode("utf-8", "replace"))[-4 * 1024 * 1024:]

    def notice(self, pattern, timeout=30):
        if not re.search(pattern, ANSI.sub("", self.transcript), re.I):
            self.expect("(?i)" + pattern, timeout=timeout)
        return ANSI.sub("", self.transcript)

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
            tail = typed[-min(16, width // 2):]
            pattern = r"Command: [^\r\n]*" + re.escape(tail) + r"[^\r\n]*[\s\S]*\x1b\[\?25h"
            frame = check_layout(self.send(key, pattern, raw=True), width)
            assert tail in frame, "typed command is hidden"
        if exits:
            return self.send("\r", SHELL)
        return check_layout(self.send("\r", r"Command: \x1b[^\r\n]*[\s\S]*\x1b\[\?25h", raw=True), width)

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


@contextmanager
def gated_editor(folder):
    with tempfile.TemporaryDirectory(prefix="gate-", dir=folder) as directory:
        directory = Path(directory)
        wrapper = directory / "gated_huffman.c"
        wrapper.write_text(r'''
#include <pthread.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
static ssize_t gated_write(int, const void *, size_t);
static int gated_join(pthread_t, void **);
#define write gated_write
#define pthread_join gated_join
#include "huffman.c"
#undef write
#undef pthread_join
static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t gate_thread;
static int gate_blocked;

static ssize_t gated_write(int fd, const void *data, size_t bytes)
{
    struct stat info;
    if (fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink &&
        lseek(fd, 0, SEEK_CUR) == 0) {
        int ready = atoi(getenv("MOON_TEST_READY"));
        int release = atoi(getenv("MOON_TEST_RELEASE"));
        pthread_mutex_lock(&gate_mutex);
        gate_thread = pthread_self();
        gate_blocked = 1;
        pthread_mutex_unlock(&gate_mutex);
        struct pollfd input = {release, POLLIN, 0};
        char token;
        int passed = write(ready, "W", 1) == 1 && poll(&input, 1, 10000) == 1 &&
                     read(release, &token, 1) == 1;
        pthread_mutex_lock(&gate_mutex);
        gate_blocked = 0;
        pthread_mutex_unlock(&gate_mutex);
        if (!passed) { errno = ETIMEDOUT; return -1; }
    }
    return write(fd, data, bytes);
}

static int gated_join(pthread_t thread, void **result)
{
    pthread_mutex_lock(&gate_mutex);
    int blocked = gate_blocked && !pthread_equal(pthread_self(), gate_thread);
    pthread_mutex_unlock(&gate_mutex);
    if (blocked) write(atoi(getenv("MOON_TEST_READY")), "J", 1);
    return pthread_join(thread, result);
}
''')
        executable = directory / "moon"
        sources = [str(path) for path in sorted(Path.cwd().glob("*.c"))
                   if path.name != "huffman.c" and not path.name.startswith(".")]
        subprocess.run([*shlex.split(os.environ.get("CC", "cc")), "-Wall", "-Wextra",
                        "-std=gnu99", "-D_GNU_SOURCE", "-pthread", "-I.", *sources,
                        str(wrapper), "-o", str(executable)], check=True, timeout=30)
        ready, child_ready = os.pipe()
        child_release, release = os.pipe()
        descriptors = [ready, child_ready, child_release, release]
        session = None
        try:
            os.set_inheritable(child_ready, True)
            os.set_inheritable(child_release, True)
            session = Session(80, executable, {"MOON_TEST_READY": str(child_ready),
                                               "MOON_TEST_RELEASE": str(child_release)})
            for fd in (child_ready, child_release):
                os.close(fd)
                descriptors.remove(fd)
            yield session, ready, release
        finally:
            if session is not None:
                session.close()
            for fd in descriptors:
                os.close(fd)


def gate_event(session, fd, token):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        readable = select.select([fd, session.fd], [], [], max(0, deadline - time.monotonic()))[0]
        if fd in readable:
            assert os.read(fd, 1) == token, "unexpected background gate event"
            return
        if session.fd in readable:
            session.record(os.read(session.fd, 65536))
    raise AssertionError("background gate timed out")


def check_layout(frame, width):
    if "\x1b[?25h" in frame:
        frame = frame[:frame.rfind("\x1b[?25h") + len("\x1b[?25h")]
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
    for name in ("q", "c", "u"):
        assert re.search(r"\b" + name + r"\b", text), f"help missing {name} at {width} columns"
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
        for command in ("a blocked", "d 1", "i 1 blocked", "o /nope/blocked.txt", f"c {folder / 'dirty.mhf'}"):
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
            for name in ("o", "p", "a", "d", "i", "s", "q", "v", "c", "u", "status", "cancel"):
                assert re.search(r"\n\s*" + name + r"\s", text), "help omitted " + name
            assert "no file open" not in text, "help requires an open file"
        assert "no file open" in ANSI.sub("", session.send("p\n"))
        assert re.search(r"no (active|background) job", ANSI.sub("", session.send("status\n")))
        assert re.search(r"no (active|background) job", ANSI.sub("", session.send("cancel\n")))
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
        session.raw()
        for key in literal:
            session.visual(key)
        assert file.read_bytes() == b"alpha\nbeta\n", "literal typing saved without Ctrl+O"
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


def huffman_roundtrip(folder, fullscreen):
    file = folder / "huffman input.txt"
    archive = folder / "huffman archive.mhf"
    output = folder / "huffman output.txt"
    original = b"alpha\nbeta\nno-final-newline"
    file.write_bytes(original)
    for path in (archive, output):
        path.unlink(missing_ok=True)
    session = Session(80)
    try:
        if fullscreen:
            session.visual(f'edit "{file}"\n')
            session.visual("\x0c")
            command = session.command
        else:
            session.send("edit\n")
            session.send(f"o {file}\n")
            command = lambda text: session.send(text + "\n")
        session.transcript = ""
        text = command(f'c "{archive}"')
        assert re.search(r"compressing|compression complete", ANSI.sub("", text), re.I), "c did not start compression"
        session.notice(r"compression complete")
        assert archive.is_file() and archive.stat().st_size > 0, "compression produced no archive"
        assert file.read_bytes() == original, "compression changed the source"
        if not fullscreen:
            session.send("q\n", SHELL)
            session.restored()
            session.send("edit\n")
        session.transcript = ""
        command(f'u "{archive}" "{output}"')
        session.notice(r"decompression complete")
        actual = output.read_bytes()
        assert actual == original, f"Huffman round trip changed saved bytes: {actual!r}"
        if not fullscreen:
            command(f"o {file}")
        for text, target in ((f'c "{archive}"', archive), (f'u "{archive}" "{output}"', output)):
            contents = target.read_bytes()
            session.transcript = ""
            command(text)
            session.notice(r"exists")
            assert target.read_bytes() == contents, "Huffman overwrote an existing destination"
        if fullscreen:
            session.command("q", exits=True)
        else:
            session.send("q\n", SHELL)
        session.restored()
    finally:
        session.close()


def huffman_background(folder):
    file = folder / "background.txt"
    archive = folder / "background.mhf"
    output = folder / "background.out"
    line = bytes(33 + ((n * 37 + n // 7) % 94) for n in range(4095)) + b"\n"
    original = line * 128
    file.write_bytes(original)
    with gated_editor(folder) as (session, ready, release):
        session.visual(f"edit {file}\n")
        session.visual("\x0c")
        session.transcript = ""
        session.command(f"c {archive}")
        gate_event(session, ready, b"W")
        session.notice(r"compressing [1-9][0-9]*%")
        text = session.command("status")
        assert re.search(r"compressing \d+%", text, re.I), "status omitted job progress"
        text = session.command(f"c {folder / 'busy.mhf'}")
        assert re.search(r"busy|already|in progress", text, re.I), "second job was accepted"
        session.send("\x0c", r"Editing; \^L commands[\s\S]*\x1b\[\?25h", raw=True)
        frame = session.send("Z", re.escape("Z" + line[:30].decode()) + r"[\s\S]*\x1b\[\?25h", raw=True)
        check_layout(frame, 80)
        session.raw()
        assert "compression complete" not in session.transcript.lower(), "editing did not run during compression"
        session.send("\x0f", r"saved file[\s\S]*\x1b\[\?25h", raw=True)
        assert file.read_bytes() == b"Z" + original, "background compression blocked saving"
        os.write(release, b"1")
        session.notice(r"compression complete")
        session.send("\x0c", r"Command: [^\r\n]*\x1b\[\?25h", raw=True)
        session.transcript = ""
        session.command(f"u {archive} {output}")
        gate_event(session, ready, b"W")
        os.write(release, b"1")
        session.notice(r"decompression complete")
        assert output.read_bytes() == original, "compression did not preserve its original snapshot"
        session.command("q", exits=True)
        session.restored()


def huffman_cleanup(folder):
    file = folder / "background.txt"
    archive = folder / "cancelled.mhf"
    output = folder / "cancelled.out"
    malformed = folder / "malformed.mhf"
    malformed.write_bytes(b"not a Huffman archive")
    with gated_editor(folder) as (session, ready, release):
        session.send("edit\n")
        session.send(f"o {file}\n")
        before = set(folder.iterdir())
        text = session.send(f"u {folder / 'background.mhf'} {output} extra\n")
        assert re.search(r"usage", text, re.I), "u accepted an extra argument"
        assert set(folder.iterdir()) == before, "invalid u arguments created output"
        session.transcript = ""
        session.send(f"u {malformed} {output}\n")
        session.notice(r"invalid|error")
        assert set(folder.iterdir()) == before, "malformed archive left output or temporary files"
        session.transcript = ""
        session.send(f"c {archive}\n")
        gate_event(session, ready, b"W")
        session.send("cancel\n")
        os.write(release, b"1")
        session.notice(r"compression cancelled")
        assert not archive.exists(), "cancel published a partial archive"
        assert set(folder.iterdir()) == before, "cancel left temporary files"
        session.transcript = ""
        session.send(f"u {folder / 'background.mhf'} {output}\n")
        gate_event(session, ready, b"W")
        session.send("cancel\n")
        os.write(release, b"1")
        session.notice(r"decompression cancelled")
        assert set(folder.iterdir()) == before, "decompression cancel left output or temporary files"
        session.transcript = ""
        session.send(f"c {archive}\n")
        gate_event(session, ready, b"W")
        os.write(session.fd, b"q\n")
        gate_event(session, ready, b"J")
        os.write(release, b"1")
        session.expect(SHELL)
        session.restored()
        session.visual(f"edit {file}\n")
        session.visual("\x0c")
        session.command(f"c {archive}")
        gate_event(session, ready, b"W")
        os.write(session.fd, b"\x18")
        gate_event(session, ready, b"J")
        os.write(release, b"1")
        session.expect(SHELL)
        session.restored()
        assert set(folder.iterdir()) == before, "fullscreen exit left output or temporary files"
        assert set(folder.iterdir()) == before, "quit left background output or temporary files"
        session.send("edit\n")
        session.send("status\n")
        session.send("q\n", SHELL)
        session.restored()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="moon-command-mode-") as directory:
        folder = Path(directory)
        huffman_roundtrip(folder, False)
        huffman_roundtrip(folder, True)
        huffman_background(folder)
        huffman_cleanup(folder)
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
    print("  ok   editor commands, background Huffman, snapshots, cleanup and terminal restore")
