"""What a step that has gone quiet is waiting on: every process below it, with the CPU each has used and how old it is.

A hung build prints nothing, so its `[build] all` banner is the last line anyone sees, for as long as it hangs.
After `quiet_seconds()` without a line of output, the watch names what is still running under the step, and repeats
that every as long again; it reports only and never ends anything, since a long link is quiet and busy.
A process that has sat at no CPU since the last report is waiting rather than working, and is called out: that is how
a resource compiler deadlocked on its own error output looked, for ten minutes, with nothing on the screen.
"""

from __future__ import annotations

import json
import os
import subprocess
import threading
import time
from dataclasses import dataclass
from typing import Callable

# The default quiet time before a report, in seconds; SC_DEV_STALL_SECS overrides it, and 0 turns the watch off.
DEFAULT_QUIET_S = 300.0


def quiet_seconds() -> float:
    """SC_DEV_STALL_SECS when it is a number, else the default; 0 or less means no watch."""
    try:
        return float(os.environ.get("SC_DEV_STALL_SECS", DEFAULT_QUIET_S))
    except ValueError:
        return DEFAULT_QUIET_S


def is_due(now: float, last_output: float, last_report: float | None, quiet_s: float) -> bool:
    """Whether a report is due: quiet for `quiet_s` since the last line, and since the last report as well."""
    if quiet_s <= 0 or now - last_output < quiet_s:
        return False
    return last_report is None or now - last_report >= quiet_s


@dataclass
class ProcessInfo:
    pid: int
    ppid: int
    name: str
    command: str
    # Seconds of CPU, user and kernel together; None where the platform did not say.
    cpu_s: float | None
    age_s: float | None


def _all_processes() -> list[ProcessInfo]:
    """Every process on the machine, best effort: an empty list where the snapshot itself failed."""
    if os.name == "nt":
        script = ("Get-CimInstance Win32_Process | ForEach-Object { [pscustomobject]@{ "
                  "p = $_.ProcessId; pp = $_.ParentProcessId; n = $_.Name; c = $_.CommandLine; "
                  "cpu = ($_.KernelModeTime + $_.UserModeTime) / 1e7; "
                  "age = if ($_.CreationDate) { ((Get-Date) - $_.CreationDate).TotalSeconds } else { $null } } } "
                  "| ConvertTo-Json -Compress")
        try:
            out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                                 capture_output=True, text=True, errors="replace", timeout=60).stdout
            rows = json.loads(out) if out.strip() else []
        except (OSError, subprocess.SubprocessError, ValueError):
            return []
        if isinstance(rows, dict):
            rows = [rows]
        return [ProcessInfo(pid=int(r["p"]), ppid=int(r["pp"]), name=r.get("n") or "?", command=r.get("c") or "",
                            cpu_s=r.get("cpu"), age_s=r.get("age")) for r in rows if r.get("p") is not None]

    try:
        out = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,etime=,time=,args="], capture_output=True, text=True,
                             errors="replace", timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    found = []
    for line in out.splitlines():
        fields = line.split(None, 4)
        if len(fields) < 5 or not fields[0].isdigit() or not fields[1].isdigit():
            continue
        command = fields[4]
        found.append(ProcessInfo(pid=int(fields[0]), ppid=int(fields[1]), name=os.path.basename(command.split()[0]),
                                 command=command, cpu_s=_clock_seconds(fields[3]), age_s=_clock_seconds(fields[2])))
    return found


def _clock_seconds(text: str) -> float | None:
    """`ps`'s `[[dd-]hh:]mm:ss[.ff]` as seconds; None where it is not that."""
    try:
        days, _, clock = text.rpartition("-")
        parts = [float(p) for p in clock.split(":")]
        seconds = 0.0
        for p in parts:
            seconds = seconds * 60 + p
        return seconds + (float(days) * 86400 if days else 0.0)
    except ValueError:
        return None


def process_tree(root: int) -> list[ProcessInfo]:
    """Every process below `root`, depth first, `root` itself left out."""
    everything = _all_processes()
    children: dict[int, list[ProcessInfo]] = {}
    for p in everything:
        if p.pid != p.ppid:
            children.setdefault(p.ppid, []).append(p)
    found: list[ProcessInfo] = []
    pending = [root]
    seen = {root}
    while pending:
        for child in children.get(pending.pop(), []):
            if child.pid in seen:
                continue
            seen.add(child.pid)
            found.append(child)
            pending.append(child.pid)
    return found


def _span(seconds: float | None) -> str:
    if seconds is None:
        return "?"
    minutes, s = divmod(int(seconds), 60)
    return f"{minutes}m {s:02d}s" if minutes else f"{seconds:.1f}s"


def describe(label: str, quiet_s: float, tree: list[ProcessInfo], idle: set[int]) -> str:
    """The report: how long the step has been quiet, then one line per process below it, the idle ones marked."""
    lines = [f"[dev.py] {label}: no output for {_span(quiet_s)}. Still running under it:"]
    if not tree:
        lines.append("[dev.py]   nothing could be listed: the step itself is the one waiting")
    for p in tree:
        command = p.command if len(p.command) <= 160 else p.command[:157] + "..."
        mark = "  <- no CPU since the last look" if p.pid in idle else ""
        cpu = "?" if p.cpu_s is None else f"{p.cpu_s:.1f}s"
        lines.append(f"[dev.py]   {p.name} (pid {p.pid}), up {_span(p.age_s)}, cpu {cpu}{mark}")
        if command:
            lines.append(f"[dev.py]       {command}")
    if idle:
        lines.append("[dev.py] A process that used no CPU in that time is waiting rather than working; the step is "
                     "most likely stuck there.")
    return "\n".join(lines)


class StallWatch:
    """Reports what a quiet child is running, through `write`, until `stop()`; `touch()` on every line of output."""

    def __init__(self, pid: int, label: str, write: Callable[[str], None], quiet_s: float | None = None,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self._pid = pid
        self._label = label
        self._write = write
        self._quiet_s = quiet_seconds() if quiet_s is None else quiet_s
        self._clock = clock
        self._last_output = clock()
        self._last_report: float | None = None
        self._last_cpu: dict[int, float] = {}
        self._stopped = threading.Event()
        self._thread: threading.Thread | None = None

    def touch(self) -> None:
        self._last_output = self._clock()

    def start(self) -> None:
        if self._quiet_s <= 0:
            return
        self._thread = threading.Thread(target=self._run, name="dev.py stall watch", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stopped.set()
        if self._thread is not None:
            self._thread.join(timeout=5)

    def check(self) -> bool:
        """One look: reports where one is due, and says whether it did."""
        now = self._clock()
        if not is_due(now, self._last_output, self._last_report, self._quiet_s):
            return False
        tree = process_tree(self._pid)
        # Idle is judged against the previous report, so the first one marks only what has used no CPU at all.
        idle = {p.pid for p in tree if p.cpu_s is not None and p.cpu_s - self._last_cpu.get(p.pid, 0.0) < 0.05}
        self._last_cpu = {p.pid: p.cpu_s for p in tree if p.cpu_s is not None}
        self._last_report = now
        try:
            self._write(describe(self._label, now - self._last_output, tree, idle))
        except Exception:
            pass  # a report that cannot be written must not take the step with it
        return True

    def _run(self) -> None:
        # Looks a few times per quiet period, so a report lands within a fraction of it of being due.
        interval = max(1.0, min(15.0, self._quiet_s / 10))
        while not self._stopped.wait(interval):
            try:
                self.check()
            except Exception:
                pass
