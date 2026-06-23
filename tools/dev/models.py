"""Plain data containers shared across the dev tooling.

These are frozen dataclasses with no behavior — just typed bags of fields that
the helper modules produce and consume. Keeping them dumb keeps the rest of the
tooling explicit: functions take and return these, nothing hides logic in a
method.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Preset:
    """A buildable preset, resolved from CMakePresets.json.

    `build_dir` is where CMake configures into (build/<configure_preset>);
    artifact paths are discovered from the CMake File API rather than guessed,
    so there is deliberately no bin_dir here.
    """

    name: str  # build preset name (what `cmake --build --preset` takes)
    configure_preset: str
    build_dir: Path
    build_type: str  # CMAKE_BUILD_TYPE, e.g. "RelWithDebInfo"


@dataclass(frozen=True)
class Target:
    """A CMake target discovered via the File API."""

    name: str
    kind: str  # "EXECUTABLE", "STATIC_LIBRARY", ...
    artifact: Path | None  # absolute path to the primary built artifact, if any


@dataclass(frozen=True)
class StepResult:
    """Outcome of a single captured subprocess step.

    `step_type` is the kind of step ("configure"/"build"/"test"); `name` is the
    specific thing it acted on (a target, "all", or a test binary). Together they
    drive the banner and the log-file name.
    """

    step_type: str
    name: str
    command: list[str]
    returncode: int
    duration_s: float
    stdout_log: Path
    stderr_log: Path
    timed_out: bool = False

    @property
    def ok(self) -> bool:
        return self.returncode == 0 and not self.timed_out


@dataclass(frozen=True)
class TestSummary:
    """Parsed totals from a JUnit XML report.

    `assertions` is the total number of checks evaluated (the nexus runner emits
    it; synthesized single-case sidecars report 0).
    """

    binary: str
    tests: int
    failures: int
    errors: int
    skipped: int
    time_s: float
    assertions: int = 0
