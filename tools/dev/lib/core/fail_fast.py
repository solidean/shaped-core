"""Once something is red, the whole command fails soon after, rather than running on to the end.

The first failure starts a grace period, GRACE_S long.
Within it everything keeps running, so a command that is red for two reasons reports both.
When it ends, every child dev.py still runs is killed, and a step that would start after that does not.

A command that wants every result whatever fails, `build --keep-going` above all, turns this off with `disable()`.
A phase whose job is to look at what failed, such as ship's run of red tests at the change's base, runs inside `exempt()`.
"""

from __future__ import annotations

import contextlib
import os
import subprocess
import sys
import threading
import time
from collections.abc import Iterator

#: How long after the first failure the command keeps going.
GRACE_S = 10.0

#: What a step reports when the grace period ended it, or kept it from starting: neither a pass nor a timeout.
ABORTED_RC = 125

_lock = threading.Lock()
# DEV_FAIL_FAST=0 is how a dev.py that starts another one asks it to run everything, ship's run at the change's base above all.
_enabled = os.environ.get("DEV_FAIL_FAST", "1") != "0"
_exempt_depth = 0
_first: tuple[float, str] | None = None
_aborted = False
_timer: threading.Timer | None = None
_live: set[subprocess.Popen] = set()
_killed: set[subprocess.Popen] = set()


def disable() -> None:
    """Every step runs to its end whatever fails, for the rest of this process."""
    global _enabled
    with _lock:
        _enabled = False
        if _timer is not None:
            _timer.cancel()


@contextlib.contextmanager
def exempt() -> Iterator[None]:
    """A phase that runs whatever failed before it, and whose own failures end nothing, with the grace state cleared."""
    global _exempt_depth, _first, _aborted, _timer
    with _lock:
        _exempt_depth += 1
        if _timer is not None:
            _timer.cancel()
        _timer, _first, _aborted = None, None, False
    try:
        yield
    finally:
        with _lock:
            _exempt_depth -= 1


def note_failure(label: str) -> None:
    """Something is red; the first call starts the grace period, later ones change nothing."""
    global _first, _timer
    with _lock:
        if not _enabled or _exempt_depth > 0 or _first is not None:
            return
        _first = (time.monotonic(), label)
        _timer = threading.Timer(GRACE_S, _abort)
        _timer.daemon = True
        _timer.start()
    print(f"[dev.py] {label} is red: everything still running gets {GRACE_S:.0f} s, then the command fails",
          file=sys.stderr, flush=True)


def aborted() -> bool:
    """Whether the grace period has ended, so nothing new may start."""
    with _lock:
        return _aborted


def reason() -> str:
    """Why a step was ended or never started, for its log and its summary."""
    with _lock:
        label = _first[1] if _first is not None else "a step"
    return f"{label} failed {GRACE_S:.0f} s before, and the command fails once that grace has passed"


@contextlib.contextmanager
def tracked(proc: subprocess.Popen) -> Iterator[None]:
    """`proc` is killed if the grace period ends while it runs."""
    with _lock:
        _live.add(proc)
        late = _aborted
    if late:
        with _lock:
            _killed.add(proc)
        _kill(proc)
    try:
        yield
    finally:
        with _lock:
            _live.discard(proc)


def was_killed(proc: subprocess.Popen) -> bool:
    """Whether the grace period is what ended `proc`, judged after it exited."""
    with _lock:
        return proc in _killed


def enabled() -> bool:
    """Whether a failure ends this command, so a test binary is worth telling to stop early too."""
    with _lock:
        return _enabled and _exempt_depth == 0


#: Lines in a test step's output that say a test is red, while the binary still runs: nexus's judged checks, and the
#: build server client's verdict per binary.
FAILURE_MARKERS = ("nexus: check failed", "] FAILED ")
_SERVER_FAILED = " failed ("


def is_failure_line(line: str) -> bool:
    """Whether a line of a test step's output says that something is red."""
    if any(m in line for m in FAILURE_MARKERS):
        return True
    return line.lstrip().startswith("ccs: test ") and _SERVER_FAILED in line


def _abort() -> None:
    global _aborted
    with _lock:
        if not _enabled or _exempt_depth > 0:
            return
        _aborted = True
        victims = list(_live)
        _killed.update(victims)
    for proc in victims:
        _kill(proc)


def _kill(proc: subprocess.Popen) -> None:
    """Ends `proc` and everything it started: a build's compilers and a test binary's helpers alike."""
    if proc.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
    with contextlib.suppress(OSError):
        proc.kill()
