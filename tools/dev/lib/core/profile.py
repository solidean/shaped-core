"""Job-level profiling: record every unit of work a dev.py run causes, then hand it to a trace viewer.

A job is one thing with a wall-clock start and end — a captured subprocess, a compile edge, an in-process phase.
Recording is off unless `--profile` asked for it, and `span` hands back a shared no-op then, so an instrumented call site costs nothing on the default path.

Times are absolute Unix epoch seconds throughout.
That is what lets two profiles from two separate invocations merge without renormalizing, and it is what the compile sidecars already record.

Jobs divide by **origin**, and the two halves are laid out differently because they are shaped differently.
A `driver` job is one dev.py timed on its own call stack, so the driver's jobs nest exactly and are placed by containment depth.
An `external` job was harvested or fanned out — a compile edge, a child's per-file lint — so those overlap arbitrarily and are placed by greedy lane allocation instead.

A job that encloses another is an **aggregate**: its time is its children's, so adding it to a per-type total would double-count.
`classify` marks that, which is what lets the summary report the leaves separately from the containers they sit in.
"""

from __future__ import annotations

import atexit
import bisect
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

SCHEMA_VERSION = 2

# Names the directory a child process drops its job fragments into.
# A child cannot reach this process's list, so it writes its own file and the parent merges it in `write`.
ENV_FRAGMENT_DIR = "SC_DEV_PROFILE_DIR"

# Jobs this close together share a lane instead of opening a new one.
# Two steps that end and start "at the same time" differ by microseconds of Python bookkeeping, and a lane per such pair would bury the real parallelism.
DEFAULT_LANE_EPSILON_S = 1e-4

FORMATS = ("jobs", "chrome-tracing")
LANE_MODES = ("global", "per-type")

# The two rows-of-tracks a trace is split into: dev.py's own nested timeline, and everything that fanned out under it.
DRIVER_GROUP = "dev.py"
EXTERNAL_GROUP = "jobs"


@dataclass
class Job:
    """One unit of profiled work.

    `type` is the kind of job ("build", "compile", "test", "lint", ...) and doubles as the lane pool under `per-type` allocation.
    `origin` is "driver" for a job this process timed itself and "external" for a harvested or fanned-out one; it decides which layout the job gets.
    `extra` is free-form and is carried through to the trace viewer's argument pane, so it must stay JSON-serializable.
    `group`, `lane`, `depth` and `aggregate` are filled by `classify` and `allocate_lanes`.
    """

    name: str
    type: str
    start: float
    end: float
    extra: dict = field(default_factory=dict)
    origin: str = "driver"
    group: str = ""
    lane: int = -1
    depth: int = 0
    aggregate: bool = False

    @property
    def duration_s(self) -> float:
        return self.end - self.start

    @property
    def is_leaf(self) -> bool:
        return not self.aggregate

    def as_dict(self) -> dict:
        d = {
            "name": self.name,
            "type": self.type,
            "origin": self.origin,
            "start": round(self.start, 6),
            "end": round(self.end, 6),
            "dur": round(self.duration_s, 6),
            "leaf": self.is_leaf,
        }
        if self.lane >= 0:
            d["group"] = self.group
            d["lane"] = self.lane
        if self.origin == "driver":
            d["depth"] = self.depth
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
            origin=str(d.get("origin") or "driver"),
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
    """Add already-timed jobs in bulk — a sidecar harvest, or a child's fragment.

    These are stamped `external`, whatever they claimed: they fan out in parallel and cannot be placed on this process's call stack, so they get lanes rather than nesting.
    """
    if not _recording or not jobs:
        return
    for job in jobs:
        job.origin = "external"
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
# Containment and lanes
# ---------------------------------------------------------------------------

def _containment_key(job: Job) -> tuple[float, float]:
    """Start ascending, end descending — so a container always sorts ahead of what it contains."""
    return (job.start, -job.end)


def classify(jobs: list[Job]) -> None:
    """Mark which jobs enclose another, and how deep each driver job sits.

    Only a driver job can be an aggregate, and that restriction is the load-bearing part.
    Enclosing another job means something for a driver span, which wraps its children on one call stack: a build step really does account for the compiles inside it.
    Between two external jobs it means nothing at all: a six-second compile encloses a fast one that ran on a different core.
    Reading that as structure would file most of the fan-out as "container" and empty out the leaf table.

    Containment is **strict**, and is resolved over distinct intervals rather than over jobs.
    Two jobs with the same interval are peers, not one inside the other, so a step whose only child exactly matches it stays a leaf rather than swallowing it.

    Depth is likewise only computed for driver jobs.
    They come from one call stack, so a stack walk gives their true depth; external jobs overlap arbitrarily and are laid out by lane instead.
    """
    ordered = sorted(jobs, key=_containment_key)
    for job in ordered:
        job.aggregate = False
        job.depth = 0

    # Distinct intervals, ordered so that an interval precedes everything it could contain.
    spans = sorted({(j.start, j.end) for j in ordered}, key=lambda se: (se[0], -se[1]))
    n = len(spans)
    if n > 1:
        rank = {se: i for i, se in enumerate(spans)}
        starts = [s for s, _ in spans]

        # Sparse table of minimum end over an index range, so "does anything starting inside my window also finish inside it" is one lookup rather than a scan.
        levels = [[e for _, e in spans]]
        width = 2
        while width <= n:
            prev, half = levels[-1], width // 2
            levels.append([min(prev[i], prev[i + half]) for i in range(n - width + 1)])
            width *= 2

        for job in ordered:
            if job.origin != "driver":
                continue
            i = rank[(job.start, job.end)]
            last = bisect.bisect_right(starts, job.end) - 1
            if last <= i:
                continue # nothing distinct from this job's own interval starts inside it
            level = (last - i).bit_length() - 1
            row = levels[level]
            if min(row[i + 1], row[last - (1 << level) + 1]) <= job.end:
                job.aggregate = True

    # Unwind to the innermost span that actually contains this one, rather than to the first that merely started earlier.
    # Adjacent siblings are the case that needs it: the previous one can end a hair after the next begins.
    # A start-versus-end test alone would read that as nesting and bury the sibling a row too deep.
    eps = DEFAULT_LANE_EPSILON_S
    open_jobs: list[Job] = []
    for job in (j for j in ordered if j.origin == "driver"):
        while open_jobs and not (job.start >= open_jobs[-1].start - eps and job.end <= open_jobs[-1].end + eps):
            open_jobs.pop()
        job.depth = len(open_jobs)
        open_jobs.append(job)


def allocate_lanes(jobs: list[Job], *, mode: str = "global",
                   epsilon: float = DEFAULT_LANE_EPSILON_S) -> int:
    """Pack jobs into as few lanes as their overlap requires, filling each job's `group` and `lane`.

    For the external jobs, which is who this is for: they fan out with no thread to attribute them to, so the lanes are reconstructed.
    Sorting containers ahead of what they contain keeps a wrapping job in a lower lane than its contents.
    `epsilon` lets a job start a hair before its lane's previous job ended without opening a new lane.
    Under `per-type` each job type gets its own pool, and so its own track in the trace.
    Returns the total lane count across all pools.
    """
    lane_ends: dict[str, list[float]] = {}
    for job in sorted(jobs, key=_containment_key):
        job.group = EXTERNAL_GROUP if mode == "global" else job.type
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


def lay_out(jobs: list[Job], *, mode: str = "global",
            epsilon: float = DEFAULT_LANE_EPSILON_S) -> int:
    """Classify every job, then place each one on a track: driver jobs by depth, external jobs by lane.

    Driver and external are always allocated separately, so a compile edge can never be pushed down a row by the step that spawned it.
    Returns the total number of tracks.
    """
    classify(jobs)
    driver = [j for j in jobs if j.origin == "driver"]
    for job in driver:
        job.group = DRIVER_GROUP
        job.lane = job.depth
    depths = 1 + max((j.depth for j in driver), default=-1)
    return depths + allocate_lanes([j for j in jobs if j.origin != "driver"], mode=mode, epsilon=epsilon)


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


@dataclass(frozen=True)
class ProfileSummary:
    """The two tables a profile is worth reading as.

    `leaves` is the work itself, and its rows are disjoint, so the trailing `all leaves` row is a total that means something.
    `containers` is the jobs that enclose other jobs — the run's structure — whose time is already counted in `leaves` and must not be added to it.
    Keeping them apart is the difference between "compiling costs 85 s" and an unreadable column where `invocation` looks like the expensive part.
    """

    leaves: list[TypeStat]
    containers: list[TypeStat]
    count: int


def _by_type(jobs: list[Job]) -> list[TypeStat]:
    by_type: dict[str, list[Job]] = {}
    for job in jobs:
        by_type.setdefault(job.type, []).append(job)
    stats = [
        TypeStat(type=t, count=len(js), total_s=sum(j.duration_s for j in js), span_s=union_span(js))
        for t, js in by_type.items()
    ]
    stats.sort(key=lambda s: -s.total_s)
    return stats


def summarize(jobs: list[Job]) -> ProfileSummary:
    """Split the profile into leaf work and the containers around it, each per type and heaviest first.

    Classifies first, so this is safe to call on a raw job list.
    """
    if not jobs:
        return ProfileSummary(leaves=[], containers=[], count=0)

    classify(jobs)
    leaves = [j for j in jobs if j.is_leaf]
    containers = [j for j in jobs if j.aggregate]

    leaf_stats = _by_type(leaves)
    if leaf_stats:
        leaf_stats.append(TypeStat(
            type="all leaves", count=len(leaves),
            total_s=sum(j.duration_s for j in leaves), span_s=union_span(leaves),
        ))
    return ProfileSummary(leaves=leaf_stats, containers=_by_type(containers), count=len(jobs))


# ---------------------------------------------------------------------------
# Output formats
# ---------------------------------------------------------------------------

def to_document(jobs: list[Job], *, lane_mode: str, track_count: int, argv: list[str]) -> dict:
    """The raw job document `--profile` writes by default, containers ahead of what they contain."""
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "dev-profile",
        "argv": argv,
        "created": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "lane_mode": lane_mode,
        "track_count": track_count,
        "jobs": [j.as_dict() for j in sorted(jobs, key=_containment_key)],
    }


def to_chrome_trace(jobs: list[Job], *, argv: list[str] | None = None) -> dict:
    """Render laid-out jobs as Chrome Trace Event Format, which https://ui.perfetto.dev loads directly.

    Each group becomes a process and each track a thread, so the viewer shows dev.py's nested timeline and the fanned-out work as separate, independently collapsible blocks.
    The driver process is emitted first so it lands at the top, since it is the one that explains the shape of the run.
    Timestamps are microseconds relative to the earliest job, as the format requires; the absolute epoch stays in each event's args.
    A zero-length job is widened to 1 us, since a slice of no width cannot be clicked.
    """
    if not jobs:
        return {"displayTimeUnit": "ms", "traceEvents": []}

    t0 = min(j.start for j in jobs)
    # The driver group first, then the rest alphabetically.
    groups = sorted({j.group for j in jobs}, key=lambda g: (g != DRIVER_GROUP, g))
    pid_of = {g: i + 1 for i, g in enumerate(groups)}

    events: list[dict] = []
    for g in groups:
        events.append({"ph": "M", "pid": pid_of[g], "tid": 0, "name": "process_name",
                       "args": {"name": g or EXTERNAL_GROUP}})
    for g, lane in sorted({(j.group, j.lane) for j in jobs}):
        row = f"depth {lane}" if g == DRIVER_GROUP else f"lane {lane}"
        events.append({"ph": "M", "pid": pid_of[g], "tid": lane, "name": "thread_name",
                       "args": {"name": row}})

    for j in sorted(jobs, key=lambda j: (j.start, -j.end)):
        events.append({
            "ph": "X",
            "pid": pid_of[j.group],
            "tid": max(j.lane, 0),
            "name": j.name,
            "cat": j.type,
            "ts": round((j.start - t0) * 1e6),
            "dur": max(1, round(j.duration_s * 1e6)),
            "args": {"type": j.type, "leaf": j.is_leaf, "start_epoch": round(j.start, 6), **j.extra},
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

    A job with no recorded `origin` — a schema-1 profile, written before the driver/external split existed — reads back as `driver`.
    That is the safe default for the driver's own steps and wrong for a harvested compile, so re-record rather than re-lane an old file.
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


def emit(jobs: list[Job], path: Path, *, fmt: str, lane_mode: str, argv: list[str]) -> ProfileSummary:
    """Lay `jobs` out, write them to `path` in `fmt`, and return the summary of what was written."""
    track_count = lay_out(jobs, mode=lane_mode)
    doc = (to_chrome_trace(jobs, argv=argv) if fmt == "chrome-tracing"
           else to_document(jobs, lane_mode=lane_mode, track_count=track_count, argv=argv))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    return summarize(jobs)


def _collect_fragments() -> list[Job]:
    """Every job a cooperating child recorded, then drop the fragment directory.

    A child's jobs were driver jobs to the child, but to us they are fan-out under a single step, so they are restamped external and laid out in lanes.
    """
    global _fragment_dir, _owns_fragment_dir

    if _fragment_dir is None or not _owns_fragment_dir:
        return []
    jobs: list[Job] = []
    for f in sorted(_fragment_dir.glob("*.json")):
        jobs.extend(load(f))
    for job in jobs:
        job.origin = "external"
    shutil.rmtree(_fragment_dir, ignore_errors=True)
    os.environ.pop(ENV_FRAGMENT_DIR, None)
    _fragment_dir = None
    _owns_fragment_dir = False
    return jobs


def write() -> ProfileSummary:
    """Emit the configured profile, children's fragments included, and return its summary.

    Empty without writing when nothing is recording, so an atexit hook can call it unconditionally.
    """
    if not _recording or _path is None:
        return ProfileSummary(leaves=[], containers=[], count=0)
    with _lock:
        jobs = list(_jobs)
    jobs.extend(_collect_fragments())
    return emit(jobs, _path, fmt=_fmt, lane_mode=_lane_mode, argv=_argv)


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
