"""Compile and link jobs, recovered from what a build already left behind.

Nothing extra is instrumented here.
`diag-launcher` wraps every compiler and linker invocation on the native presets and writes a `<output>.diag.json` next to each artifact, absolute timestamps included.
So a build step's per-TU and per-link timing is on disk the moment the step returns, and harvesting is a filtered read.

`mark` is taken before the step and `harvest` after it, because the sidecars accumulate across builds and only the ones this step rewrote are ours.
Emscripten presets carry no launcher and fall back to the tail of `.ninja_log`, whose edge times are relative to the ninja invocation and are anchored against the step's own end.
"""

from __future__ import annotations

import json
import os
import re
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from ..core.profile import Job

# Sidecar mtimes come from the same clock as the step timestamps, but filesystem timestamp granularity is coarser.
# The cheap mtime pass only prefilters; `finished_at` inside the record is what actually decides.
_MTIME_SLACK_S = 2.0

# CMake object paths: build/<preset>/<dir>/CMakeFiles/<target>.dir/<source>.obj
_OBJECT_RE = re.compile(r"CMakeFiles[\\/](?P<target>[^\\/]+)\.dir[\\/](?P<source>.+)$")

_OBJECT_SUFFIXES = {".o", ".obj"}


@dataclass(frozen=True)
class BuildMark:
    """Where a build step started, in the two terms harvesting needs.

    `ninja_log_size` is only read on the Emscripten fallback path, where entries have to be told apart by position rather than by timestamp.
    """

    started_at: float
    ninja_log_size: int


def mark(build_dir: Path) -> BuildMark:
    """Snapshot a build dir immediately before a build step runs."""
    try:
        size = (build_dir / ".ninja_log").stat().st_size
    except OSError:
        size = 0
    return BuildMark(started_at=time.time(), ninja_log_size=size)


def harvest(build_dir: Path, mark: BuildMark, *, ended_at: float) -> list[Job]:
    """Every compile and link job the step that `mark` opened produced.

    Prefers the `.diag.json` sidecars, and falls back to `.ninja_log` only when a build produced none — an Emscripten preset, where no launcher is installed.
    """
    jobs = _sidecar_jobs(build_dir, mark.started_at)
    if jobs:
        return jobs
    return _ninja_jobs(build_dir, mark, ended_at=ended_at)


# ---------------------------------------------------------------------------
# diag-launcher sidecars
# ---------------------------------------------------------------------------

def _sidecar_jobs(build_dir: Path, since: float) -> list[Job]:
    jobs: list[Job] = []
    cutoff = since - _MTIME_SLACK_S
    for root, _dirs, files in os.walk(build_dir):
        for f in files:
            if not f.endswith(".diag.json"):
                continue
            p = Path(root) / f
            try:
                if p.stat().st_mtime < cutoff:
                    continue
                record = json.loads(p.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            job = _job_from_record(record, since)
            if job is not None:
                jobs.append(job)
    return jobs


def _job_from_record(record: dict, since: float) -> Job | None:
    """One sidecar record as a job, or None when it predates the step or cannot be timed.

    Both schema versions the two launchers write are accepted: the checked-in `diag-launcher.exe` still stamps 1 while the Python port declares 2, and the timing fields are identical in each.
    """
    output = str(record.get("output_path") or "")
    start = _parse_rfc3339(record.get("started_at"))
    end = _parse_rfc3339(record.get("finished_at"))
    if not output or start is None or end is None:
        return None
    if end < since:
        return None # a sidecar left by an earlier build

    name, target = _describe_output(output)
    extra = {"output": output.replace("\\", "/"), "tool": Path(str(record.get("tool", ""))).name}
    if target:
        extra["target"] = target
    if record.get("exit_code"):
        extra["exit_code"] = record["exit_code"]
    return Job(name=name, type=_job_type(output), start=start, end=end, extra=extra)


def _parse_rfc3339(value: object) -> float | None:
    """RFC 3339 with a trailing Z, as both launchers write it, to epoch seconds."""
    if not isinstance(value, str) or not value:
        return None
    try:
        return datetime.strptime(value, "%Y-%m-%dT%H:%M:%S.%fZ").replace(tzinfo=timezone.utc).timestamp()
    except ValueError:
        pass
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def _job_type(output: str) -> str:
    return "compile" if Path(output).suffix.lower() in _OBJECT_SUFFIXES else "link"


def _describe_output(output: str) -> tuple[str, str]:
    """A readable job name and the owning target, derived from a build-relative output path.

    An object path carries its source under `CMakeFiles/<target>.dir/`, which reads far better in a trace than the object path does.
    Anything else — a linked binary, or a layout the pattern does not know — falls back to the file's own stem.
    """
    m = _OBJECT_RE.search(output)
    if m is None:
        return Path(output).stem, ""
    source = m.group("source").replace("\\", "/")
    for suffix in _OBJECT_SUFFIXES:
        if source.lower().endswith(suffix):
            source = source[: -len(suffix)]
            break
    return source, m.group("target")


# ---------------------------------------------------------------------------
# .ninja_log fallback (Emscripten presets, which have no launcher)
# ---------------------------------------------------------------------------

def _ninja_jobs(build_dir: Path, mark: BuildMark, *, ended_at: float) -> list[Job]:
    """Edges from the tail of `.ninja_log`, anchored so the last one ends when the step did.

    Ninja records each edge's start and end as milliseconds since its own invocation, so the tail has to be placed against a wall clock the caller knows.
    Reading from `mark.ninja_log_size` is what isolates this run's edges; a log that shrank was recompacted mid-run and is given up on rather than guessed at.
    """
    path = build_dir / ".ninja_log"
    try:
        size = path.stat().st_size
        if size <= mark.ninja_log_size:
            return []
        with open(path, "rb") as f:
            f.seek(mark.ninja_log_size)
            tail = f.read().decode("utf-8", "replace")
    except OSError:
        return []

    edges: list[tuple[int, int, str]] = []
    for line in tail.splitlines():
        parts = line.split("\t")
        if len(parts) < 4 or line.startswith("#"):
            continue
        try:
            edges.append((int(parts[0]), int(parts[1]), parts[3]))
        except ValueError:
            continue
    if not edges:
        return []

    base = ended_at - max(e for _s, e, _o in edges) / 1000.0
    base = max(base, mark.started_at)

    jobs: list[Job] = []
    for start_ms, end_ms, output in edges:
        name, target = _describe_output(output)
        extra = {"output": output, "source": "ninja-log"}
        if target:
            extra["target"] = target
        jobs.append(Job(
            name=name, type=_job_type(output),
            start=base + start_ms / 1000.0, end=base + end_ms / 1000.0, extra=extra,
        ))
    return jobs
