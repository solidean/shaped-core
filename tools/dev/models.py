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
    toolset: str | None = None  # pinned compiler version/path (--toolset); None = preset default

    @property
    def family(self) -> str:
        """Compiler family, inferred from the configure-preset name.

        Drives how a pinned --toolset is applied: clang/gcc swap the compiler binary,
        msvc selects a Visual Studio toolset via vcvars. 'unknown' if it can't be told.
        """
        cp = self.configure_preset
        if "msvc" in cp:
            return "msvc"
        if "gcc" in cp:
            return "gcc"
        if "clang" in cp:  # includes clang-cl on Windows
            return "clang"
        if "emscripten" in cp:
            return "emscripten"
        return "unknown"

    @property
    def is_emscripten(self) -> bool:
        """Whether this preset cross-compiles to WebAssembly via Emscripten.

        Such presets need the emsdk environment (emcc on PATH for configure/build,
        node to run the resulting .js/.wasm test artifacts); see process.emsdk_env.
        Keyed off the wasm-emscripten-* configure preset / emscripten-* build preset
        naming so no extra metadata has to be threaded through.
        """
        return "emscripten" in self.configure_preset or self.name.startswith("emscripten-")


@dataclass(frozen=True)
class Target:
    """A CMake target discovered via the File API."""

    name: str
    kind: str  # "EXECUTABLE", "STATIC_LIBRARY", ...
    artifact: Path | None  # absolute path to the primary built artifact, if any


@dataclass(frozen=True)
class CompileGroup:
    """One set of compile flags shared by a subset of a target's sources.

    CMake emits a separate group per distinct flag-set, so more than one group
    means the target's translation units do not all compile the same way (e.g. a
    vendored source built with `/W0`). `sources` lists the files this group
    covers; `flags` are the raw fragment strings as CMake passes them.
    """

    language: str | None  # "CXX", "C", ...
    std: str | None  # language standard, e.g. "23"
    defines: list[str]
    includes: list[tuple[str, bool]]  # (path, is_system)
    flags: list[str]
    sources: list[str]


@dataclass(frozen=True)
class TargetFlags:
    """Resolved compile and link flags for a target, from the CMake File API.

    `compile_groups` holds one entry per distinct flag-set (see CompileGroup);
    link info is empty for targets without a link step (static libraries are
    archived, not linked).
    """

    name: str
    kind: str  # "EXECUTABLE", "STATIC_LIBRARY", ...
    compile_groups: list[CompileGroup]
    link_flags: list[str]
    link_libraries: list[str]


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
