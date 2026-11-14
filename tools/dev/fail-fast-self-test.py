#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Self-test for tools/dev/lib/core/fail_fast.py — once something is red, the command fails soon after.

The grace is shortened to a fraction of a second, so no case waits the real ten.
The children are real processes sleeping in Python, which is what a long step looks like: alive, and nowhere near done.

    uv run tools/dev/fail-fast-self-test.py [-v]
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.dev.lib.core import fail_fast, process  # noqa: E402

VERBOSE = "-v" in sys.argv
failures = 0
GRACE = 0.3


def check(condition: bool, what: str, detail: str = "") -> None:
    global failures
    if condition:
        if VERBOSE:
            print(f"ok    {what}")
        return
    failures += 1
    print(f"FAIL  {what}" + (f"\n{detail}" if detail else ""))


def reset() -> None:
    """A fresh coordinator, as a new dev.py process starts with."""
    with fail_fast._lock:
        if fail_fast._timer is not None:
            fail_fast._timer.cancel()
        fail_fast._enabled, fail_fast._exempt_depth = True, 0
        fail_fast._first, fail_fast._aborted, fail_fast._timer = None, False, None
        fail_fast._live.clear()
        fail_fast._killed.clear()
    fail_fast.GRACE_S = GRACE


def sleeper(secs: float) -> list[str]:
    return [sys.executable, "-c", f"import time; time.sleep({secs})"]


def step(build_dir: Path, name: str, cmd: list[str]):
    return process.run_step(cmd, step_type="test", name=name, build_dir=build_dir, cwd=build_dir)


def test_a_failure_ends_what_still_runs_after_the_grace(build_dir: Path) -> None:
    reset()
    slow = subprocess.Popen(sleeper(30))
    with fail_fast.tracked(slow):
        fail_fast.note_failure("red")
        check(not fail_fast.aborted(), "within the grace, nothing is ended yet")
        started = time.monotonic()
        slow.wait(timeout=10)
        took = time.monotonic() - started
    check(fail_fast.aborted(), "past the grace, the command is failing")
    check(fail_fast.was_killed(slow), "the grace is what ended the slow child")
    check(GRACE * 0.5 < took < GRACE + 5, "the child ran about as long as the grace", f"it ran {took:.2f} s")
    check("red" in fail_fast.reason(), "the reason names what was red", fail_fast.reason())


def test_a_step_after_the_grace_does_not_start(build_dir: Path) -> None:
    reset()
    first = step(build_dir, "red", [sys.executable, "-c", "raise SystemExit(3)"])
    check(first.returncode == 3, "a failing step reports its own exit code", str(first.returncode))
    during = step(build_dir, "during", sleeper(0))
    check(during.ok, "a step within the grace still runs, so a second error can still show")
    time.sleep(GRACE * 2)
    after = step(build_dir, "after", sleeper(0))
    check(after.returncode == fail_fast.ABORTED_RC, "a step past the grace is not started", str(after.returncode))
    check("not started" in after.stderr_log.read_text(encoding="utf-8"), "its log says why it never ran")


def test_a_running_step_is_killed_and_says_so(build_dir: Path) -> None:
    reset()
    fail_fast.note_failure("red")
    result = step(build_dir, "slow", sleeper(30))
    check(result.returncode == fail_fast.ABORTED_RC, "a step the grace ended reports the aborted code", str(result.returncode))
    check(result.duration_s < 10, "it did not run to its end", f"{result.duration_s:.2f} s")
    check("KILLED" in result.stderr_log.read_text(encoding="utf-8"), "its log says the grace ended it")


def test_a_red_line_starts_the_grace_before_the_step_ends(build_dir: Path) -> None:
    reset()
    printer = [sys.executable, "-c", "import time; print('[ 1.0] error {t0} nexus: check failed: x', flush=True); time.sleep(30)"]
    result = step(build_dir, "binary", printer)
    check(result.returncode == fail_fast.ABORTED_RC, "a binary that says it is red is ended after the grace", str(result.returncode))
    check(fail_fast.is_failure_line("  ccs: test shaped-graphics-test failed (ran 3.3 s, 3 test(s), 3 failed)"),
          "the build server client's verdict on a binary counts as red")
    check(not fail_fast.is_failure_line("  ccs: test clean-core-test passed (ran 1.0 s, 0 failed)"),
          "a passing binary's line does not, though it says `0 failed`")


def test_an_exempt_phase_runs_through_its_reds(build_dir: Path) -> None:
    reset()
    fail_fast.note_failure("before")
    with fail_fast.exempt():
        check(not fail_fast.aborted(), "an exempt phase starts with the grace cleared")
        fail_fast.note_failure("inside")
        time.sleep(GRACE * 2)
        check(not fail_fast.aborted(), "a red inside it ends nothing")
        check(step(build_dir, "inside", sleeper(0)).ok, "a step inside it runs")
    check(not fail_fast.aborted(), "and the command goes on after it")


def test_disabled_runs_everything(build_dir: Path) -> None:
    reset()
    fail_fast.disable()
    fail_fast.note_failure("red")
    time.sleep(GRACE * 2)
    check(not fail_fast.aborted(), "a disabled coordinator ends nothing, as --keep-going asks")
    check(not fail_fast.enabled(), "and says so, so no binary is told to stop early")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="fail-fast-self-test-") as tmp:
        build_dir = Path(tmp)
        for t in (test_a_failure_ends_what_still_runs_after_the_grace, test_a_step_after_the_grace_does_not_start,
                  test_a_running_step_is_killed_and_says_so, test_a_red_line_starts_the_grace_before_the_step_ends,
                  test_an_exempt_phase_runs_through_its_reds, test_disabled_runs_everything):
            t(build_dir)
        reset()
    print(f"fail-fast-self-test: {'all passed' if failures == 0 else f'{failures} failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
