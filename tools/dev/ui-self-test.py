#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""Self-test for tools/dev/lib/core/ui.py, the live progress region.

The region only exists as escape sequences on a stream, so it is tested by driving frames into a StringIO at a fixed
width and asserting on what came out.
`painter=False` keeps rendering synchronous, which is what makes a frame sequence deterministic enough to assert on.

The load-bearing case is `up_count_matches_frame`: every other bug in a repainting region shows up there first, because a
frame that moves the cursor up by the wrong number of lines is exactly how one eats the scrollback above it.

Run with `uv run tools/dev/ui-self-test.py`, and `-v` to list the cases as they pass.
"""

from __future__ import annotations

import io
import re
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.dev.lib.core import ui  # noqa: E402

VERBOSE = "-v" in sys.argv
FAILURES: list[str] = []

_SGR_RE = re.compile(r"\033\[[0-9;]*m")
_UP_RE = re.compile(r"\033\[(\d+)A")


def check(cond: bool, what: str) -> None:
    if cond:
        if VERBOSE:
            print(f"  ok   {what}")
    else:
        FAILURES.append(what)
        print(f"  FAIL {what}")


def fresh(width: int = 80, *, tail_lines: int = 8) -> io.StringIO:
    """A configured, painter-less region writing into a fresh buffer."""
    ui.shutdown()
    buf = io.StringIO()
    ui.configure("on", stream=buf, size=(width, 24), painter=False, tail_lines=tail_lines)
    return buf


def render(text: str) -> list[str]:
    """Replay the escape stream onto a screen and return the lines a reader would end up seeing.

    Grepping the raw bytes cannot answer the question these cases ask: erased text is still *in* the stream, so a test
    that searches it would pass on a region that never cleaned up after itself.
    Only the subset ui.py actually emits is interpreted — cursor-up, erase-line, erase-down, carriage return — and an
    unrecognised sequence is a test failure waiting to happen rather than something to ignore silently.
    """
    screen: list[str] = [""]
    row = col = 0
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "\033":
            m = re.match(r"\033\[([0-9;?]*)([A-Za-z])", text[i:])
            if m is None:
                raise AssertionError(f"unparsable escape at {i}: {text[i:i + 12]!r}")
            arg, final = m.group(1), m.group(2)
            if final == "A":
                row = max(0, row - int(arg or 1))
            elif final == "K":
                screen[row] = ""
                col = 0
            elif final == "J":
                screen[row] = screen[row][:col]
                del screen[row + 1:]
            elif final in ("m", "h", "l"):
                pass  # styling and cursor visibility do not move the cursor
            else:
                raise AssertionError(f"unhandled sequence {m.group(0)!r}")
            i += m.end()
            continue
        if ch == "\n":
            row += 1
            col = 0
            while len(screen) <= row:
                screen.append("")
        elif ch == "\r":
            col = 0
        else:
            line = screen[row].ljust(col)
            screen[row] = line[:col] + ch + line[col + 1:]
            col += 1
        i += 1
    return screen


# ---------------------------------------------------------------------------

def test_up_count_matches_frame() -> None:
    """Each frame must move up by exactly the number of newlines the previous frame wrote."""
    buf = fresh()
    with ui.step("build all", step_type="build") as s:
        for i in range(5):
            s.feed(f"[{i + 1}/5] Building object number {i}")
            ui.render_once(i)
        out = buf.getvalue()

    # Walk the stream: count newlines emitted since the last up-move, and compare with the next up-move's argument.
    parts = _UP_RE.split(out)
    # parts alternates [text, n, text, n, ...]; every n must equal the newline count of the text before it.
    ok = True
    for i in range(1, len(parts), 2):
        declared = int(parts[i])
        written = parts[i - 1].count("\n")
        if declared != written:
            ok = False
            print(f"       frame {i // 2}: moved up {declared}, previous frame wrote {written}")
    check(ok, "up_count_matches_frame")
    check(len(parts) > 1, "up_count_matches_frame: frames were actually repainted")


def test_truncation() -> None:
    """A very long line must never occupy more than one terminal row."""
    buf = fresh(width=80)
    with ui.step("build all", step_type="build") as s:
        s.feed("x" * 500)
        ui.render_once()
        out = buf.getvalue()
    longest = max((len(ln) for ln in render(out)), default=0)
    check(longest <= 79, f"truncation: longest rendered line {longest} <= 79 (the last column stays free)")


def test_success_collapses() -> None:
    """A step that succeeds leaves its summary and nothing else."""
    buf = fresh()
    with ui.step("build all", step_type="build") as s:
        s.feed("[1/2] compiling something noisy")
        s.feed("[2/2] compiling something else noisy")
        ui.render_once()
        s.finish(ok=True, summary="  all succeeded in 10 ms")
    ui.shutdown()
    kept = [ln for ln in render(buf.getvalue()) if ln.strip()]
    check(any("all succeeded" in ln for ln in kept), "success_collapses: summary is kept")
    check(not any("noisy" in ln for ln in kept), "success_collapses: tail is erased")


def test_failure_retains_tail() -> None:
    """A step that fails keeps its tail as the evidence."""
    buf = fresh()
    with ui.step("build all", step_type="build") as s:
        s.feed("error: something went wrong")
        ui.render_once()
        s.finish(ok=False, summary="  all failed in 10 ms")
    ui.shutdown()
    kept = [ln for ln in render(buf.getvalue()) if ln.strip()]
    check(any("something went wrong" in ln for ln in kept), "failure_retains_tail: tail is kept")
    check(any("all failed" in ln for ln in kept), "failure_retains_tail: summary is kept")


def test_write_line_never_tears() -> None:
    """A permanent line is always preceded by an erase, so it can never land inside a frame."""
    buf = fresh()
    with ui.step("build all", step_type="build") as s:
        s.feed("[1/3] first")
        ui.render_once()
        ui.write_line("a permanent line")
        s.feed("[2/3] second")
        ui.render_once()
        out = buf.getvalue()
    screen = render(out)
    hits = [i for i, ln in enumerate(screen) if "a permanent line" in ln]
    check(len(hits) == 1, "write_line_never_tears: appears exactly once on screen")
    live = [i for i, ln in enumerate(screen) if "build all" in ln]
    check(bool(hits) and bool(live) and hits[0] < live[0],
          "write_line_never_tears: the permanent line sits above the live region")


def test_concurrent_rows_stay_ordered() -> None:
    """Three steps feeding from three threads keep their row order and produce well-formed frames."""
    buf = fresh()
    ui.configure("on", stream=buf, size=(100, 24), painter=True, interval_s=0.01)
    steps = []
    stack = []
    for name in ("alpha", "beta", "gamma"):
        cm = ui.step(f"build {name}", step_type="build")
        stack.append(cm)
        steps.append(cm.__enter__())

    def feeder(s, tag):
        for i in range(200):
            s.feed(f"[{i}/200] {tag} line {i}")

    threads = [threading.Thread(target=feeder, args=(s, n))
               for s, n in zip(steps, ("alpha", "beta", "gamma"))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    ui.render_once()
    out = buf.getvalue()
    for cm in reversed(stack):
        cm.__exit__(None, None, None)
    ui.shutdown()

    frames = _UP_RE.split(out)
    ok = all(int(frames[i]) == frames[i - 1].count("\n") for i in range(1, len(frames), 2))
    check(ok, "concurrent_rows_stay_ordered: every frame's up-count is exact")
    screen = render(out)
    rows = [ln for ln in screen if "build " in ln]
    check(len(rows) == 3, f"concurrent_rows_stay_ordered: all three rows on screen (got {len(rows)})")
    order = [ln.split("build ")[1].split()[0] for ln in rows]
    check(order == ["alpha", "beta", "gamma"], f"concurrent_rows_stay_ordered: row order preserved {order}")
    check(max(len(ln) for ln in screen) <= 99, "concurrent_rows_stay_ordered: nothing exceeded the width")


def test_disabled_is_inert() -> None:
    """With the region off, nothing emits an escape sequence and write_line is a plain print."""
    ui.shutdown()
    buf = io.StringIO()
    ui.configure("off", stream=buf, painter=False)
    with ui.step("build all", step_type="build") as s:
        s.feed("noise that must not appear")
        ui.render_once()
        s.finish(ok=True, summary="")
    check(buf.getvalue() == "", "disabled_is_inert: nothing is written to the stream")

    out = io.StringIO()
    ui.write_line("plain", stream=out)
    check(out.getvalue() == "plain\n", "disabled_is_inert: write_line falls back to print")
    check("\033" not in out.getvalue(), "disabled_is_inert: no escape sequences")


def test_phase_counts() -> None:
    """A phase row renders its k-of-N counter and retires cleanly."""
    buf = fresh()
    with ui.phase("test", total=3) as ph:
        ph.advance("clean-core-test")
        ui.render_once()
        mid = buf.getvalue()
        ph.advance("nexus-test")
        ui.render_once()
        after_two = buf.getvalue()
    closed = buf.getvalue()
    ui.shutdown()
    check(any("[1/3]" in ln for ln in render(mid)), "phase_counts: renders [1/3] after one advance")
    check(any("[2/3]" in ln for ln in render(after_two)), "phase_counts: renders [2/3] after two")
    # Leaving the phase retires its row, so the counter must not survive on screen.
    check(not any("[2/3]" in ln for ln in render(closed)), "phase_counts: the row is erased on close")


TESTS = [
    test_up_count_matches_frame,
    test_truncation,
    test_success_collapses,
    test_failure_retains_tail,
    test_write_line_never_tears,
    test_concurrent_rows_stay_ordered,
    test_disabled_is_inert,
    test_phase_counts,
]


def main() -> None:
    for t in TESTS:
        if VERBOSE:
            print(f"{t.__name__}:")
        try:
            t()
        except Exception as e:  # a raising case is a failure like any other
            FAILURES.append(f"{t.__name__} raised {e!r}")
            print(f"  FAIL {t.__name__} raised {e!r}")
        finally:
            ui.shutdown()

    if FAILURES:
        print(f"\nui-self-test: FAIL ({len(FAILURES)} failure(s))")
        sys.exit(1)
    print(f"ui-self-test: OK ({len(TESTS)} cases)")


if __name__ == "__main__":
    main()
