"""Live terminal progress: a repainting region below the permanent transcript.

Additive to capture, exactly as mirroring is.
The region is drawn, erased and forgotten, while every log file, JSON sidecar, JUnit report and profile record is written
identically whether it ran or not — so `build_diag` and `test_diag` read the same artifacts either way.

Enabled only on a real terminal that can do ANSI.
Piped, redirected or in CI, every entry point here is a no-op and the terse capture-only trace stands unchanged, which is
what an agent driving dev.py through a pipe sees.
Zero dependencies: hand-rolled CSI and SGR, the same way console.py is.

One invariant holds the whole thing up: the up-count must equal the number of newlines the last frame wrote.
Every line is therefore truncated to the terminal width in *columns* before it is styled, so a long compiler diagnostic
can never wrap and desynchronise the count, and one row of terminal is always one line of ours.
Columns rather than characters, because a wide character occupies two cells and a line measured in characters fits by
that count and still wraps on screen.
"""

from __future__ import annotations

import collections
import contextlib
import os
import platform
import shutil
import sys
import threading
import time
import unicodedata
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from typing import TextIO

from . import console

_CSI = "\033["
_HIDE_CURSOR = f"{_CSI}?25l"
_SHOW_CURSOR = f"{_CSI}?25h"

_SPINNER_UNICODE = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
_SPINNER_ASCII = "-\\|/"
_BAR_UNICODE = ("█", "░")
_BAR_ASCII = ("#", ".")

# Environment variables that mean "a CI runner", any one of which is enough.
# CI is detected rather than inferred from the absence of a TTY: some runners allocate a pty, and a repainting region
# baked into a log file nobody can scrub is worse than no progress at all.
_CI_VARS = ("CI", "GITHUB_ACTIONS", "GITLAB_CI", "TF_BUILD", "BUILDKITE",
            "JENKINS_URL", "TEAMCITY_VERSION", "APPVEYOR", "CIRCLECI")

Sniffer = Callable[[str], "tuple[int, int] | None"]
"""Given one line of a step's output, return (done, total) when it reports progress, else None."""


def _ninja_sniffer() -> Sniffer:
    """Read ninja's `[done/total]` edge counter off the live stream.

    Shares logs.NINJA_EDGE_RE with ninja_built_count, so what an edge line looks like stays defined once.
    That function counts lines in a finished log, which is a different question from where the build is right now.
    """
    from .logs import NINJA_EDGE_RE

    def sniff(line: str) -> tuple[int, int] | None:
        m = NINJA_EDGE_RE.match(line)
        return (int(m.group(1)), int(m.group(2))) if m else None

    return sniff


_SNIFFERS: dict[str, Callable[[], Sniffer]] = {"build": _ninja_sniffer}


@dataclass
class _Row:
    label: str
    step_type: str
    started: float
    tail: collections.deque
    sniffer: Sniffer | None = None
    done: int | None = None
    total: int | None = None
    # A phase row is a k-of-N counter rather than a live step, and it sorts above the steps it counts.
    is_phase: bool = False
    note: str = ""


# Two locks, never one.
# `_io_lock` is held across writes to the terminal, and a terminal write can block for as long as the far end is paused
# — a selected Windows console, a stopped tty, a slow ssh link.
# `_rows_lock` guards only the row list and the row fields, so the pump threads in process.py can hand a line to a row
# without ever waiting on the screen.
# That is what keeps _pump's rule intact: a blocked terminal must not stop the child's pipes being drained.
# Order, wherever both are held: _io_lock first, then _rows_lock.
# Nothing takes them the other way round.
_io_lock = threading.RLock()
_rows_lock = threading.RLock()
_enabled = False
_stream: TextIO | None = None
_size: tuple[int, int] | None = None
_tail_lines = 8
_interval_s = 0.08
_rows: list[_Row] = []
_painted = 0
_suspended = 0
_cursor_hidden = False
_unicode = True
_painter: threading.Thread | None = None
_stop = threading.Event()
_win_console_mode: int | None = None
_prev_sigint = None
_sigint_installed = False


# ---------------------------------------------------------------------------
# Setup and teardown
# ---------------------------------------------------------------------------

def is_ci() -> bool:
    """Whether this looks like a CI runner."""
    return any(os.environ.get(v) for v in _CI_VARS)


def _enable_vt() -> bool:
    """Make sure the terminal interprets ANSI, and say whether it will.

    Windows Terminal and Windows 11 conhost already do; a legacy console needs the mode bit set, and one that refuses it
    would print raw escapes, so a failure here turns the whole region off rather than corrupting the output.
    """
    global _win_console_mode
    if platform.system() != "Windows":
        return True
    try:
        import ctypes

        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-12)  # STD_ERROR_HANDLE: the region lives on stderr
        mode = ctypes.c_uint32()
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        _win_console_mode = mode.value
        return bool(kernel32.SetConsoleMode(handle, mode.value | 0x0004))  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
    except (OSError, AttributeError, ImportError):
        return False


def _resolve(mode: str, *, own_stream: bool) -> bool:
    """Decide whether the region runs.

    `own_stream` means the caller supplied its own sink, so the real terminal's capabilities are not the question being
    asked — that is the self-test, writing frames into a buffer.
    """
    if mode == "off":
        return False
    if mode == "on":
        return True if own_stream else _enable_vt()  # an explicit --progress still needs a terminal that can do it
    env = os.environ.get("SC_DEV_UI", "").lower()
    if env in ("0", "false", "no"):
        return False
    if env in ("1", "true", "yes"):
        return _enable_vt()
    if os.environ.get("TERM", "") == "dumb":
        return False
    if is_ci():
        return False
    # Both streams, the same discriminator console.configure("auto") uses: a redirected stdout is the agent case, and the
    # region must stay out of the data it is reading.
    if not (sys.stdout.isatty() and sys.stderr.isatty()):
        return False
    return _enable_vt()


def configure(
    mode: str = "auto",
    *,
    stream: TextIO | None = None,
    size: tuple[int, int] | None = None,
    interval_s: float = 0.08,
    painter: bool = True,
    tail_lines: int = 8,
) -> None:
    """Resolve once whether the live region runs, and start its painter.

    `stream` and `size` exist for the self-test, which drives frames into a StringIO at a fixed width.
    `painter=False` leaves rendering to explicit render_once() calls, so a test sees deterministic frames and no thread.
    """
    global _enabled, _stream, _size, _tail_lines, _interval_s, _painter, _unicode
    _stop_painter()  # a second configure must not leave the first one's thread repainting against the new state
    _enabled = _resolve(mode, own_stream=stream is not None)
    _stream = stream if stream is not None else sys.stderr
    _size = size
    _tail_lines = tail_lines
    _interval_s = interval_s
    _unicode = _stream_handles_unicode()
    if not _enabled:
        return

    _install_sigint()
    if painter:
        _stop.clear()
        _painter = threading.Thread(target=_paint_loop, name="dev-ui", daemon=True)
        _painter.start()


def _stop_painter() -> None:
    """Stop the painter thread and wait for it to leave, so no two threads ever repaint against one _painted."""
    global _painter
    _stop.set()
    if _painter is not None and _painter.is_alive() and _painter is not threading.current_thread():
        _painter.join(timeout=1.0)
    _painter = None


def _stream_handles_unicode() -> bool:
    """Whether the spinner and bar glyphs survive the stream's encoding, so a cp437 console degrades instead of throwing."""
    enc = getattr(_stream, "encoding", None)
    if not enc:
        return True
    try:
        (_SPINNER_UNICODE + _BAR_UNICODE[0]).encode(enc)
        return True
    except (UnicodeEncodeError, LookupError):
        return False


def _install_sigint() -> None:
    """Restore the terminal on Ctrl-C, then let the previous handler do what it did before.

    A KeyboardInterrupt during a build must not leave the cursor hidden, and atexit alone does not run early enough to
    keep the traceback out of the region.
    Installed once: chaining a new handler onto the previous one at every configure would stack them.
    """
    global _prev_sigint, _sigint_installed
    import signal

    if _sigint_installed:
        return
    try:
        previous = signal.getsignal(signal.SIGINT)
    except (ValueError, AttributeError):
        return

    def handler(signum, frame):
        shutdown()
        if callable(previous):
            previous(signum, frame)
        else:
            raise KeyboardInterrupt

    try:
        signal.signal(signal.SIGINT, handler)
    except (ValueError, OSError):
        return  # not the main thread, so there is no handler to install
    _prev_sigint, _sigint_installed = previous, True


def _restore_sigint() -> None:
    """Put back whatever handler was there before, so a configure/shutdown cycle leaves the process as it found it."""
    global _prev_sigint, _sigint_installed
    if not _sigint_installed:
        return
    import signal

    try:
        signal.signal(signal.SIGINT, _prev_sigint)
    except (ValueError, OSError, TypeError):
        pass
    _prev_sigint, _sigint_installed = None, False


def enabled() -> bool:
    return _enabled


def shutdown() -> None:
    """Erase the region, show the cursor, stop the painter.

    Idempotent, and safe to call from atexit or from a signal handler.
    """
    global _enabled, _cursor_hidden
    if not _enabled:
        return
    _stop_painter()
    with _io_lock:
        _erase()
        if _cursor_hidden:
            _write(_SHOW_CURSOR)
            _cursor_hidden = False
        _flush()
        with _rows_lock:
            _rows.clear()
        _enabled = False
    _restore_console_mode()
    _restore_sigint()


def _restore_console_mode() -> None:
    if _win_console_mode is None:
        return
    try:
        import ctypes

        kernel32 = ctypes.windll.kernel32
        kernel32.SetConsoleMode(kernel32.GetStdHandle(-12), _win_console_mode)
    except (OSError, AttributeError, ImportError):
        pass


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def _write(s: str) -> None:
    try:
        _stream.write(s)
    except (OSError, ValueError):
        pass  # the terminal went away mid-frame; the run itself must not care


def _flush() -> None:
    try:
        _stream.flush()
    except (OSError, ValueError):
        pass


def _width() -> int:
    if _size is not None:
        return _size[0]
    return shutil.get_terminal_size((80, 24)).columns


def _cells(ch: str) -> int:
    """Terminal cells one character occupies.

    East Asian wide and fullwidth forms take two, which is the whole reason the region cannot measure in characters:
    a path or an identifier echoed back by a diagnostic would wrap, and a wrapped line is counted once and drawn twice.
    """
    return 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1


def _columns(text: str) -> int:
    """Terminal cells `text` occupies."""
    return sum(_cells(ch) for ch in text)


def _fit(text: str, width: int) -> str:
    """Cut one raw line to `width` printable columns, before any styling is applied.

    Columns rather than characters, since those differ and only the column count decides how many rows a line takes.
    Control characters are dropped and tabs expanded first: a compiler diagnostic carries both, and either would make the
    line occupy a different number of terminal rows than the frame counted on.
    """
    text = text.replace("\t", "    ")
    text = "".join(ch for ch in text if ch == " " or ch.isprintable())
    used = 0
    for i, ch in enumerate(text):
        cells = _cells(ch)
        if used + cells > width:
            return text[:i]
        used += cells
    return text


def _fmt_elapsed(seconds: float) -> str:
    m, s = divmod(int(seconds), 60)
    return f"{m}:{s:02d}"


def _bar(done: int, total: int, width: int) -> str:
    full, empty = _BAR_UNICODE if _unicode else _BAR_ASCII
    filled = 0 if total <= 0 else max(0, min(width, round(width * done / total)))
    return full * filled + empty * (width - filled)


def _spinner(tick: int) -> str:
    frames = _SPINNER_UNICODE if _unicode else _SPINNER_ASCII
    return frames[tick % len(frames)]


def _row_lines(row: _Row, tick: int, width: int) -> list[str]:
    """The visual lines one row occupies: its status line, then its tail."""
    elapsed = _fmt_elapsed(time.monotonic() - row.started)
    head = f"{_spinner(tick)} {row.label}"
    if row.done is not None and row.total:
        head += f"  [{row.done}/{row.total}]"
        # The bar takes whatever is left after the text and the clock, and is dropped entirely on a narrow terminal.
        room = width - _columns(head) - len(elapsed) - 12
        if room >= 8:
            pct = round(100 * row.done / row.total)
            head += f"  {_bar(row.done, row.total, min(24, room))} {pct:>3}%"
    if row.note:
        head += f"  {row.note}"
    pad = max(1, width - _columns(head) - len(elapsed) - 4)
    lines = [console.dim(_fit(f"  {head}{' ' * pad}{elapsed}", width - 1))]
    for entry in row.tail:
        lines.append(console.dim(_fit(f"  | {entry}", width - 1)))
    return lines


def _erase() -> None:
    """Remove the whole region, leaving the cursor where it began.

    Callers hold _io_lock.
    """
    global _painted
    if _painted:
        _write(f"{_CSI}{_painted}A{_CSI}J")
        _painted = 0


def _paint(tick: int = 0) -> None:
    """Draw every row and record how many lines it took.

    Callers hold _io_lock.
    The frame is composed under _rows_lock and then written with that lock released, so a terminal that blocks mid-frame
    never holds up a pump thread calling feed().
    """
    global _painted, _cursor_hidden
    if _suspended:
        return
    width = _width()
    with _rows_lock:
        if not _rows:
            return
        lines: list[str] = []
        for row in sorted(_rows, key=lambda r: not r.is_phase):
            lines.extend(_row_lines(row, tick, width))
    if not _cursor_hidden:
        _write(_HIDE_CURSOR)
        _cursor_hidden = True
    for line in lines:
        _write(f"\r{_CSI}2K{line}\n")
    _write(f"{_CSI}J")
    _painted = len(lines)
    _flush()


def render_once(tick: int = 0) -> None:
    """Repaint synchronously.

    What the painter thread calls, and what the self-test drives instead of it.
    """
    if not _enabled:
        return
    with _io_lock:
        _erase()
        _paint(tick)


def _paint_loop() -> None:
    """Repaint on a timer rather than per line.

    A build delivers thousands of lines a second and a full repaint each would cost more than the build, so the painter
    coalesces them.
    The timer is also the only thing that can animate at all: a configure step emits nothing for twenty seconds, and its
    spinner and clock have no event to hang off.
    """
    tick = 0
    while not _stop.wait(_interval_s):
        tick += 1
        with _rows_lock:
            idle = not _rows
        if idle or _suspended:
            continue
        with _io_lock:
            _erase()
            _paint(tick)


# ---------------------------------------------------------------------------
# Permanent output
# ---------------------------------------------------------------------------

def write_line(text: str, *, stream: TextIO | None = None) -> None:
    """Print one permanent line that cannot land inside a frame.

    A plain print() when the region is off, so call sites read the same in either mode.
    """
    if not _enabled:
        print(text, file=stream if stream is not None else sys.stderr)
        return
    with _io_lock:
        _erase()
        _write(text + "\n")
        _paint()
        _flush()


@contextlib.contextmanager
def suspend() -> Iterator[None]:
    """Park the region for the duration — a mirrored step, or anything else that owns the screen."""
    global _suspended
    if not _enabled:
        yield
        return
    with _io_lock:
        _erase()
        _flush()
        _suspended += 1
    try:
        yield
    finally:
        with _io_lock:
            _suspended -= 1


# ---------------------------------------------------------------------------
# Steps and phases
# ---------------------------------------------------------------------------

class Step:
    """One row of the region: a status line plus a rolling tail of what the step is saying right now.

    Every method is thread-safe and a no-op when the region is off, except finish, which always emits its summary line —
    that line is the step's permanent record and exists in both modes.
    """

    def __init__(self, row: _Row | None) -> None:
        self._row = row

    def feed(self, line: str) -> None:
        """Take one line of the step's output.

        Called from the pump threads, so it must never touch the terminal itself.
        """
        row = self._row
        if row is None:
            return
        line = line.rstrip("\n")
        with _rows_lock:
            row.tail.append(line)
            if row.sniffer is not None:
                progress = row.sniffer(line)
                if progress is not None:
                    row.done, row.total = progress

    def set_progress(self, done: int | None, total: int | None) -> None:
        if self._row is None:
            return
        with _rows_lock:
            self._row.done, self._row.total = done, total

    def finish(self, *, ok: bool, summary: str) -> None:
        """Retire the row: on success it collapses to `summary` alone, on failure its tail is kept as the evidence."""
        row = self._row
        if row is None:
            if summary:
                write_line(summary)
            return
        self._row = None
        with _io_lock:
            _erase()
            # Snapshot and unlink under the rows lock, then write with it released.
            with _rows_lock:
                tail = [] if ok else list(row.tail)
                if row in _rows:
                    _rows.remove(row)
            for entry in tail:
                _write(console.dim(_fit(f"  | {entry}", _width() - 1)) + "\n")
            if summary:
                _write(summary + "\n")
            _paint()
            _flush()


def open_step(label: str, *, step_type: str, active: bool = True) -> Step:
    """Open a row and hand back its handle, which the caller must finish().

    The caller owns the lifetime because the summary a step collapses to is only known once it has exited, well after any
    `with` block around the child process has closed.
    `active=False` returns an inert handle that still emits its summary — that is the mirrored step, which owns the
    screen itself and must not also hold a row.
    """
    if not _enabled or not active:
        return Step(None)
    factory = _SNIFFERS.get(step_type)
    row = _Row(label=label, step_type=step_type, started=time.monotonic(),
               tail=collections.deque(maxlen=_tail_lines), sniffer=factory() if factory else None)
    with _rows_lock:
        _rows.append(row)
    return Step(row)


@contextlib.contextmanager
def step(label: str, *, step_type: str) -> Iterator[Step]:
    """Open a row for one step.

    An escaping exception retires it, so a crash never leaves a live row behind.
    """
    handle = open_step(label, step_type=step_type)
    try:
        yield handle
    finally:
        handle.finish(ok=False, summary="")


class Phase:
    """A k-of-N counter shown above the steps it counts: test binaries, or check gates."""

    def __init__(self, row: _Row | None) -> None:
        self._row = row

    def advance(self, note: str = "") -> None:
        if self._row is None:
            return
        with _rows_lock:
            self._row.done = (self._row.done or 0) + 1
            self._row.note = note

    def close(self) -> None:
        row = self._row
        if row is None:
            return
        self._row = None
        with _io_lock:
            _erase()
            with _rows_lock:
                if row in _rows:
                    _rows.remove(row)
            _paint()


def open_phase(label: str, *, total: int) -> Phase:
    """Open a counter row and hand back its handle, which the caller must close().

    The counterpart to open_step, for a loop whose `continue` branches would make a `with` block awkward to wrap around.
    """
    if not _enabled or total <= 0:
        return Phase(None)
    row = _Row(label=label, step_type="phase", started=time.monotonic(),
               tail=collections.deque(maxlen=0), done=0, total=total, is_phase=True)
    with _rows_lock:
        _rows.append(row)
    return Phase(row)


@contextlib.contextmanager
def phase(label: str, *, total: int) -> Iterator[Phase]:
    """Open a k-of-N counter row, when there is more than nothing to count."""
    handle = open_phase(label, total=total)
    try:
        yield handle
    finally:
        handle.close()
