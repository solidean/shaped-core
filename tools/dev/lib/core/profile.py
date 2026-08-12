"""Job-level profiling: record every unit of work a dev.py run causes, then hand it to a trace viewer.

A job is one thing with a wall-clock start and end — a captured subprocess, a compile edge, an in-process phase.
Recording is off unless `--profile` asked for it, and `span` hands back a shared no-op then, so an instrumented call site costs nothing on the default path.

Times are absolute Unix epoch seconds throughout.
That is what lets two profiles from two separate invocations merge without renormalizing, and it is what the compile sidecars already record.

Lanes are reconstructed, not observed: nothing here has a thread or core to attribute a job to, so `allocate_lanes` greedily packs overlapping jobs into as few lanes as their overlap requires.
"""

from __future__ import annotations

import atexit
import json
import os
import shutil
import tempfile
import threading
import time
from contextlib import contextmanager
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

SCHEMA_VERSION = 1

# Names the directory a child process drops its job fragments into.
# A child cannot reach this process's list, so it writes its own file and the parent merges it in `write`.
ENV_FRAGMENT_DIR = "SC_DEV_PROFILE_DIR"

# Jobs this close together share a lane instead of opening a new one.
# Two steps that end and start "at the same time" differ by microseconds of Python bookkeeping, and a lane per such pair would bury the real parallelism.
DEFAULT_LANE_EPSILON_S = 1e-4

FORMATS = ("jobs", "chrome-tracing")
LANE_MODES = ("global", "per-type")


@dataclass
class Job:
    """One unit of profiled work.

    `type` is the kind of job ("build", "compile", "test", "lint", ...) and doubles as the lane pool under `per-type` allocation.
    `extra` is free-form and is carried through to the trace viewer's argument pane, so it must stay JSON-serializable.
    `group` and `lane` are empty until `allocate_lanes` fills them.
    """

    name: str
    type: str
    start: float
    end: float
    extra: dict = field(default_factory=dict)
    group: str = ""
    lane: int = -1

    @property
    def duration_s(self) -> float:
        return self.end - self.start

    def as_dict(self) -> dict:
        d = {
            "name": self.name,
            "type": self.type,
            "start": round(self.start, 6),
            "end": round(self.end, 6),
            "dur": round(self.duration_s, 6),
        }
        if self.lane >= 0:
            d["group"] = self.group
            d["lane"] = self.lane
        if self.extra:
            d["extra"] = self.extra
        return d

    @classmethod
    def from_dict(cls, d: dict) -> Job:
        return cls(
            name=str(d.get("name", "")),
            type=str(d.get("type", "")),
            start=float(d.get("start", 0.0)),
            end=float(d.get("end", 0.0)),
            extra=d.get("extra") or {},
        )


# ---------------------------------------------------------------------------
# The recorder — process-wide, set once, like console.configure and configure_mirroring
# ---------------------------------------------------------------------------

_lock = threading.Lock()
_jobs: list[Job] = []
_recording = False
_path: Path | None = None
_fmt = "jobs"
_lane_mode = "global"
_argv: list[str] = []
_fragment_dir: Path | None = None
_owns_fragment_dir = False


def configure(path: str | Path, *, fmt: str = "jobs", lane_mode: str = "global",
              argv: list[str] | None = None) -> None:
    """Start recording, and say where the profile lands and in what shape.

    Called once from dev.py's main(); `write` is what actually emits, from an atexit hook.
    Also creates the fragment directory and exports it, so any child that cooperates records into the same profile.
    """
    global _recording, _path, _fmt, _lane_mode, _argv, _fragment_dir, _owns_fragment_dir

    _recording = True
    _path = Path(path)
    _fmt = fmt if fmt in FORMATS else "jobs"
    _lane_mode = lane_mode if lane_mode in LANE_MODES else "global"
    _argv = list(argv or [])

    _fragment_dir = Path(tempfile.mkdtemp(prefix="dev-profile-"))
    _owns_fragment_dir = True
    os.environ[ENV_FRAGMENT_DIR] = str(_fragment_dir)


def configure_from_env() -> bool:
    """Record into the parent dev.py's profile, when this process was spawned under one.

    Returns whether recording was turned on, which is False for every run that did not ask for a profile.
    The fragment is written from an atexit hook, so a child only has to call this once and then use `span` / `record` normally.
    """
    global _recording, _fragment_dir

    raw = os.environ.get(ENV_FRAGMENT_DIR)
    if not raw:
        return False
    d = Path(raw)
    if not d.is_dir():
        return False

    _recording = True
    _fragment_dir = d
    atexit.register(_write_fragment)
    return True


def enabled() -> bool:
    return _recording


def fragment_env() -> dict[str, str]:
    """The environment overlay a child needs to record into this profile, empty when not recording.

    `run_step` merges this into any explicit env it was handed, since an MSVC environment is rebuilt from VsDevCmd output rather than inherited.
    """
    if not _recording or _fragment_dir is None:
        return {}
    return {ENV_FRAGMENT_DIR: str(_fragment_dir)}


def record(name: str, *, type: str, start: float, end: float, extra: dict | None = None) -> None:
    """Add one finished job.

    Safe to call from any thread, and a no-op when not recording.
    """
    if not _recording:
        return
    job = Job(name=name, type=type, start=start, end=end, extra=extra or {})
    with _lock:
        _jobs.append(job)


def add_jobs(jobs: list[Job]) -> None:
    """Add already-timed jobs in bulk — a sidecar harvest, or a merged fragment."""
    if not _recording or not jobs:
        return
    with _lock:
        _jobs.extend(jobs)


class _NullSpan:
    """What `span` returns when nothing is recording: enter and exit do nothing at all."""

    __slots__ = ()

    def __enter__(self) -> _NullSpan:
        return self

    def __exit__(self, *exc: object) -> bool:
        return False


_NULL_SPAN = _NullSpan()


@contextmanager
def _timed_span(name: str, type: str, extra: dict | None):
    start = time.time()
    try:
        yield
    finally:
        record(name, type=type, start=start, end=time.time(), extra=extra)


def span(name: str, *, type: str, extra: dict | None = None):
    """Time an in-process phase as a job — the gaps a subprocess-only profile would leave unexplained.

    The job is recorded even when the body raises, so a failed run still accounts for its time.
    """
    if not _recording:
        return _NULL_SPAN
    return _timed_span(name, type, extra)


# ---------------------------------------------------------------------------
# Lanes
# ---------------------------------------------------------------------------

def allocate_lanes(jobs: list[Job], *, mode: str = "global",
                   epsilon: float = DEFAULT_LANE_EPSILON_S) -> int:
    """Pack jobs into as few lanes as their overlap requires, filling each job's `group` and `lane`.

    Sorting by start ascending and end descending puts a container ahead of everything it contains.
    That is what lands a build step in a lower lane than the compile edges it spawns, so the result reads as a flame chart rather than a shuffle.
    `epsilon` lets a job start a hair before its lane's previous job ended without opening a new lane.
    Under `per-type` each job type gets its own pool, so a compile edge and a test binary never share a lane.
    Returns the total lane count across all pools.
    """
    lane_ends: dict[str, list[float]] = {}
    for job in sorted(jobs, key=lambda j: (j.start, -j.end)):
        job.group = "all" if mode == "global" else job.type
        ends = lane_ends.setdefault(job.group, [])
        for i, end in enumerate(ends):
            if end <= job.start + epsilon:
                job.lane = i
                ends[i] = max(end, job.end)
                break
        else:
            job.lane = len(ends)
            ends.append(job.end)
    return sum(len(e) for e in lane_ends.values())


# ---------------------------------------------------------------------------
# Aggregation
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class TypeStat:
    """What one job type cost, in the two numbers that answer different questions.

    `total_s` sums every job's duration, so 32 compilers running for a second each read as 32 seconds.
    `span_s` is the union of their intervals, so the same 32 read as one — that is the wall clock the type is actually responsible for.
    The gap between them is the parallelism.
    """

    type: str
    count: int
    total_s: float
    span_s: float

    @property
    def parallelism(self) -> float:
        return self.total_s / self.span_s if self.span_s > 0 else 0.0


def union_span(jobs: list[Job]) -> float:
    """Wall-clock seconds at least one of `jobs` was running, counting overlap once."""
    if not jobs:
        return 0.0
    total = 0.0
    current_start, current_end = None, 0.0
    for job in sorted(jobs, key=lambda j: j.start):
        if current_start is None:
            current_start, current_end = job.start, job.end
        elif job.start > current_end:
            total += current_end - current_start
            current_start, current_end = job.start, job.end
        else:
            current_end = max(current_end, job.end)
    if current_start is not None:
        total += current_end - current_start
    return total


def summarize(jobs: list[Job]) -> list[TypeStat]:
    """Per-type totals, heaviest first, with an `all` row carrying the run's own wall clock."""
    if not jobs:
        return []
    by_type: dict[str, list[Job]] = {}
    for job in jobs:
        by_type.setdefault(job.type, []).append(job)

    stats = [
        TypeStat(type=t, count=len(js), total_s=sum(j.duration_s for j in js), span_s=union_span(js))
        for t, js in by_type.items()
    ]
    stats.sort(key=lambda s: -s.total_s)
    stats.append(TypeStat(
        type="all", count=len(jobs),
        total_s=sum(j.duration_s for j in jobs), span_s=union_span(jobs),
    ))
    return stats


# ---------------------------------------------------------------------------
# Output formats
# ---------------------------------------------------------------------------

def to_document(jobs: list[Job], *, lane_mode: str, lane_count: int, argv: list[str]) -> dict:
    """The raw job document `--profile` writes by default, jobs sorted by start."""
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "dev-profile",
        "argv": argv,
        "created": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "lane_mode": lane_mode,
        "lane_count": lane_count,
        "jobs": [j.as_dict() for j in sorted(jobs, key=lambda j: (j.start, -j.end))],
    }


def to_chrome_trace(jobs: list[Job], *, argv: list[str] | None = None) -> dict:
    """Render allocated jobs as Chrome Trace Event Format, which https://ui.perfetto.dev loads directly.

    Each lane pool becomes a process and each lane a thread, so a per-type allocation shows one labelled track per kind of work.
    Timestamps are microseconds relative to the earliest job, as the format requires; the absolute epoch stays in each event's args.
    A zero-length job is widened to 1 us, since a slice of no width cannot be clicked.
    """
    if not jobs:
        return {"displayTimeUnit": "ms", "traceEvents": []}

    t0 = min(j.start for j in jobs)
    groups = sorted({j.group for j in jobs})
    pid_of = {g: i + 1 for i, g in enumerate(groups)}

    events: list[dict] = []
    for g in groups:
        events.append({"ph": "M", "pid": pid_of[g], "tid": 0, "name": "process_name",
                       "args": {"name": "dev.py" if g in ("", "all") else g}})
    for g, lane in sorted({(j.group, j.lane) for j in jobs}):
        events.append({"ph": "M", "pid": pid_of[g], "tid": lane, "name": "thread_name",
                       "args": {"name": f"lane {lane}"}})

    for j in sorted(jobs, key=lambda j: (j.start, -j.end)):
        events.append({
            "ph": "X",
            "pid": pid_of[j.group],
            "tid": max(j.lane, 0),
            "name": j.name,
            "cat": j.type,
            "ts": round((j.start - t0) * 1e6),
            "dur": max(1, round(j.duration_s * 1e6)),
            "args": {"type": j.type, "start_epoch": round(j.start, 6), **j.extra},
        })

    doc: dict = {"displayTimeUnit": "ms", "traceEvents": events}
    if argv:
        doc["otherData"] = {"argv": " ".join(argv)}
    return doc


# ---------------------------------------------------------------------------
# Reading, merging, writing
# ---------------------------------------------------------------------------

def load(path: Path) -> list[Job]:
    """Read jobs back from any profile this module writes; an unreadable or foreign file yields none.

    A converted trace is accepted as well as a job document, so rendering a profile as `chrome-tracing` is not a dead end for merging it later.
    Chrome timestamps are relative to the capture, so the absolute epoch each event carries in its args is what a re-merge needs.
    """
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []

    if isinstance(data, dict) and "traceEvents" in data:
        return _jobs_from_chrome(data["traceEvents"])

    raw = data.get("jobs") if isinstance(data, dict) else data
    if not isinstance(raw, list):
        return []
    return [Job.from_dict(d) for d in raw if isinstance(d, dict)]


def _jobs_from_chrome(events: object) -> list[Job]:
    """Complete events from a trace this module emitted, back as jobs.

    Only events carrying `start_epoch` can be placed on an absolute timeline, so a foreign trace without it is skipped rather than shifted to the wrong era.
    """
    if not isinstance(events, list):
        return []
    jobs: list[Job] = []
    for e in events:
        if not isinstance(e, dict) or e.get("ph") != "X":
            continue
        args = e.get("args") if isinstance(e.get("args"), dict) else {}
        start = args.get("start_epoch")
        if not isinstance(start, (int, float)):
            continue
        extra = {k: v for k, v in args.items() if k not in ("type", "start_epoch")}
        jobs.append(Job(
            name=str(e.get("name", "")),
            type=str(e.get("cat") or args.get("type") or ""),
            start=float(start),
            end=float(start) + float(e.get("dur", 0)) / 1e6,
            extra=extra,
        ))
    return jobs


def emit(jobs: list[Job], path: Path, *, fmt: str, lane_mode: str, argv: list[str]) -> int:
    """Allocate lanes over `jobs` and write them to `path` in `fmt`; returns the job count."""
    lane_count = allocate_lanes(jobs, mode=lane_mode)
    doc = (to_chrome_trace(jobs, argv=argv) if fmt == "chrome-tracing"
           else to_document(jobs, lane_mode=lane_mode, lane_count=lane_count, argv=argv))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    return len(jobs)


def _collect_fragments() -> list[Job]:
    """Every job a cooperating child recorded, then drop the fragment directory."""
    global _fragment_dir, _owns_fragment_dir

    if _fragment_dir is None or not _owns_fragment_dir:
        return []
    jobs: list[Job] = []
    for f in sorted(_fragment_dir.glob("*.json")):
        jobs.extend(load(f))
    shutil.rmtree(_fragment_dir, ignore_errors=True)
    os.environ.pop(ENV_FRAGMENT_DIR, None)
    _fragment_dir = None
    _owns_fragment_dir = False
    return jobs


def write() -> list[TypeStat]:
    """Emit the configured profile, children's fragments included, and return its per-type summary.

    Empty without writing when nothing is recording, so an atexit hook can call it unconditionally.
    """
    if not _recording or _path is None:
        return []
    with _lock:
        jobs = list(_jobs)
    jobs.extend(_collect_fragments())
    emit(jobs, _path, fmt=_fmt, lane_mode=_lane_mode, argv=_argv)
    return summarize(jobs)


def _write_fragment() -> None:
    """A child's atexit hook: hand this process's jobs to the parent through the fragment directory."""
    if _fragment_dir is None:
        return
    with _lock:
        jobs = list(_jobs)
    if not jobs:
        return
    name = f"frag-{os.getpid()}-{os.urandom(4).hex()}.json"
    try:
        (_fragment_dir / name).write_text(
            json.dumps({"jobs": [j.as_dict() for j in jobs]}), encoding="utf-8"
        )
    except OSError:
        pass # the parent's profile loses this child's detail, which is not worth failing the run over
