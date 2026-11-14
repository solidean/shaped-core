#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# ///

"""Self-test for tools/dev/lib/core/stall_report.py — what a step that has gone quiet is still running.

The clock is injected, so no case waits for a quiet period to pass.
The one real process is a child blocked on its stdin, which is what a hung tool looks like: alive, and using no CPU.

Kept out of `dev.py test`, which drives the C++ nexus suites; this is Python tooling testing Python tooling.

    uv run tools/dev/stall-report-self-test.py [-v]
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.dev.lib.core import stall_report  # noqa: E402

VERBOSE = "-v" in sys.argv
failures = 0


def check(condition: bool, what: str, detail: str = "") -> None:
    global failures
    if condition:
        if VERBOSE:
            print(f"ok    {what}")
        return
    failures += 1
    print(f"FAIL  {what}" + (f"\n{detail}" if detail else ""))


# ---- when a report is due -------------------------------------------------------------------------------------------

check(not stall_report.is_due(now=99, last_output=0, last_report=None, quiet_s=100), "quiet for less than the period")
check(stall_report.is_due(now=100, last_output=0, last_report=None, quiet_s=100), "quiet for the whole period")
check(not stall_report.is_due(now=150, last_output=0, last_report=100, quiet_s=100), "a report repeats no sooner")
check(stall_report.is_due(now=200, last_output=0, last_report=100, quiet_s=100), "a report repeats once as quiet again")
check(not stall_report.is_due(now=10_000, last_output=0, last_report=None, quiet_s=0), "a period of 0 is no watch")

check(stall_report._clock_seconds("01:02") == 62, "ps time mm:ss")
check(stall_report._clock_seconds("1-00:00:01") == 86401, "ps time dd-hh:mm:ss")
check(stall_report._clock_seconds("nope") is None, "ps time that is none")

# ---- the process tree and the report, over a child that waits -------------------------------------------------------

child = subprocess.Popen([sys.executable, "-c", "import sys; sys.stdin.read()"], stdin=subprocess.PIPE)
try:
    tree = stall_report.process_tree(os.getpid())
    mine = [p for p in tree if p.pid == child.pid]
    check(len(mine) == 1, "the waiting child is in this process's tree", repr(tree))
    if mine:
        check(mine[0].cpu_s is not None, "its CPU time is known")
        check(mine[0].age_s is not None and mine[0].age_s >= 0, "its age is known")

    now = [1000.0]
    written: list[str] = []
    watch = stall_report.StallWatch(os.getpid(), "build all", written.append, quiet_s=300, clock=lambda: now[0])
    check(not watch.check(), "no report before the step has been quiet for long")
    now[0] += 301
    check(watch.check(), "a report once it has")
    check(len(written) == 1 and "build all: no output for 5m 01s" in written[0], "the report says how long", "\n".join(written))
    check(len(written) == 1 and f"(pid {child.pid})" in written[0], "the report names the waiting child", "\n".join(written))
    now[0] += 150
    check(not watch.check(), "no second report within the period")
    now[0] += 150
    check(watch.check(), "a second report a period later")
    second = written[-1] if written else ""
    line = next((l for l in second.splitlines() if f"(pid {child.pid})" in l), "")
    check("<- no CPU since the last look" in line, "the second report marks the child as waiting", second)
    watch.touch()
    now[0] += 10
    check(not watch.check(), "output resets the quiet period")
finally:
    child.stdin.close()
    child.wait(timeout=30)

empty = stall_report.describe("build all", 300, [], set())
check("nothing could be listed" in empty, "a step with nothing below it says so", empty)

if failures:
    print(f"{failures} failure(s)")
    sys.exit(1)
print("stall-report self-test passed")
