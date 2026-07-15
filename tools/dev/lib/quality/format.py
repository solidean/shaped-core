"""clang-format integration: locate clang-format, check its version, and run it.

This backs `dev.py format`. clang-format output is not stable across major
versions, so the tooling pins to the major version declared in `.clang-format`'s
`Requires: clang-format >= N` header (single source of truth — the same file
that defines the style). `format_sources` runs a single clang-format invocation
through the shared `run_step`, either rewriting files in place or, in check
mode, reporting which files do not conform.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

from ..core import console
from ..core.models import StepResult
from ..core.process import response_file, run_step


class FormatSetupError(Exception):
    """Unrecoverable setup problem for a format run (clang-format missing or a
    version mismatch), carrying the message the CLI should surface."""


@dataclass(frozen=True)
class FormatResult:
    """Outcome of a `run_format` invocation, for the CLI to present.

    `nothing` flags "no files in scope" (a success). In check mode `offenders`
    lists the non-conforming files when `ok` is False.
    """

    ok: bool
    check: bool
    dirty_only: bool
    files: int
    duration_s: float = 0.0
    nothing: bool = False
    stderr_log: Path | None = None
    offenders: list[Path] = field(default_factory=list)

# Default if `.clang-format`'s `Requires:` header can't be read. Keep in sync
# with the header, which remains the authoritative source.
_DEFAULT_MAJOR = 21

_SOURCE_SUFFIXES = (".cc", ".hh")

# Common Windows install locations to try when clang-format is not on PATH,
# mirroring clangd's fallbacks.
_FALLBACK_PATHS = (
    Path(r"C:\Program Files\LLVM\bin\clang-format.exe"),
    Path(r"C:\Program Files (x86)\LLVM\bin\clang-format.exe"),
)

# clang-format --dry-run -Werror emits one diagnostic per non-conforming file:
#   path/to/foo.cc:12:5: error: code should be clang-formatted [-Wclang-format-violations]
# (the level is "warning" without -Werror, "error" with it). The leading path can
# itself contain colons on Windows ("C:\..."), so the non-greedy capture stops at
# the first ":<line>:<col>:" position, which is the real separator.
_VIOLATION_RE = re.compile(r"^(?P<file>.+?):\d+:\d+:\s+(?:error|warning):", re.MULTILINE)


def find_clang_format(explicit: str | None = None) -> str | None:
    """Locate the clang-format executable: an explicit path/name, then PATH, then
    the common LLVM install locations. Returns None if nothing usable is found."""
    if explicit:
        if Path(explicit).is_file():
            return explicit
        return shutil.which(explicit)
    found = shutil.which("clang-format")
    if found:
        return found
    for candidate in _FALLBACK_PATHS:
        if candidate.is_file():
            return str(candidate)
    return None


def clang_format_version(exe: str) -> tuple[int, ...] | None:
    """Return clang-format's version as a tuple (e.g. (21, 1, 0)), or None if it
    cannot be run or parsed."""
    try:
        result = subprocess.run([exe, "--version"], capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        return None
    m = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", result.stdout or "")
    if m is None:
        return None
    return tuple(int(g) for g in m.groups() if g is not None)


def required_major(root: Path) -> int:
    """The major clang-format version declared by `.clang-format`.

    Parses the `Requires: clang-format >= N` header so the version check enforces
    exactly what the style file declares, with no second constant to keep in
    sync. Falls back to _DEFAULT_MAJOR if the header is missing or unreadable.
    """
    try:
        text = (root / ".clang-format").read_text(encoding="utf-8")
    except OSError:
        return _DEFAULT_MAJOR
    m = re.search(r"Requires:\s*clang-format\s*>=\s*(\d+)", text)
    return int(m.group(1)) if m else _DEFAULT_MAJOR


def _git_dirty_files(root: Path) -> list[Path]:
    """Files that are git-dirty or untracked — what's reasonably part of the next
    commit. Deletions are dropped (nothing to format); renames yield their new
    path. Returns absolute paths; nonexistent entries are filtered out."""
    try:
        out = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=all"],
            cwd=str(root), capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    if out.returncode != 0:
        return []

    paths: list[Path] = []
    for line in out.stdout.splitlines():
        if len(line) < 4:
            continue
        # Porcelain v1: 2-char status field, a space, then the path; renames are
        # 'R  <old> -> <new>'. A 'D' in either status column is a deletion.
        status, rest = line[:2], line[3:]
        if "D" in status:
            continue
        path_part = rest.split(" -> ")[-1].strip().strip('"')
        p = (root / path_part).resolve()
        if p.is_file():
            paths.append(p)
    return paths


def discover_files(root: Path, *, dirty_only: bool) -> list[Path]:
    """Return the sorted list of `.cc`/`.hh` files under `libs/` to format.

    With `dirty_only`, restrict to git-dirty/untracked files (intersected with
    the same `libs/**` `.cc`/`.hh` scope).
    """
    libs = root / "libs"

    if dirty_only:
        selected = [
            p for p in _git_dirty_files(root)
            if p.suffix in _SOURCE_SUFFIXES and libs in p.parents
        ]
        return sorted(set(selected))

    found: list[Path] = []
    for dirpath, _dirnames, filenames in os.walk(libs):
        for f in filenames:
            if f.endswith(_SOURCE_SUFFIXES):
                found.append(Path(dirpath) / f)
    return sorted(found)


def format_sources(
    files: list[Path],
    *,
    root: Path,
    clang_format: str,
    check: bool,
    mirror: bool = False,
    verbose: bool = False,
) -> StepResult:
    """Run clang-format over `files` in a single invocation via run_step.

    In apply mode rewrites files in place (`-i`). In check mode runs
    `--dry-run -Werror`, which exits non-zero and names each non-conforming file
    in its output without modifying anything.
    """
    cmd = [clang_format]
    cmd += ["--dry-run", "-Werror"] if check else ["-i"]

    # a full-tree run is ~45k chars of paths -- past the Windows command-line limit
    with response_file([str(f) for f in files], prefix="clang-format-") as tail:
        return run_step(
            cmd + tail,
            step_type="format",
            name="check" if check else "apply",
            build_dir=root / "build",
            cwd=root,
            mirror=mirror,
            verbose=verbose,
        )


def run_format(
    root: Path,
    *,
    check: bool,
    dirty_only: bool,
    allow_different_version: bool,
    mirror: bool = False,
    verbose: bool = False,
) -> FormatResult:
    """Locate clang-format, enforce its version, and format the selected sources.

    Raises FormatSetupError for unrecoverable setup problems (clang-format
    missing, version undeterminable, or a major mismatch without
    `allow_different_version` — which otherwise downgrades to a yellow warning).
    Returns a FormatResult describing what happened (including the "no files in
    scope" success); the caller prints the summary (see report.summarize_format).
    """
    clang_format = find_clang_format()
    if clang_format is None:
        raise FormatSetupError(
            "clang-format not found on PATH. Install LLVM/clang-format (>= 21) or add it to PATH."
        )

    # clang-format output is not stable across major versions, so enforce the
    # major declared by .clang-format. allow_different_version downgrades the
    # mismatch to a warning instead of failing.
    have = clang_format_version(clang_format)
    need = required_major(root)
    if have is None:
        raise FormatSetupError(f"could not determine clang-format version from {clang_format!r}")
    if have[0] != need:
        have_str = ".".join(str(p) for p in have)
        msg = (f"clang-format major version {have[0]} != required {need} "
               f"(found {have_str}); formatting may differ from the pinned style")
        if allow_different_version:
            print(console.yellow(f"WARNING: {msg}"), file=sys.stderr)
        else:
            raise FormatSetupError(
                f"{msg}. Install clang-format {need}.x, or pass --allow-different-version to proceed anyway."
            )

    files = discover_files(root, dirty_only=dirty_only)
    if not files:
        return FormatResult(ok=True, check=check, dirty_only=dirty_only, files=0, nothing=True)

    result = format_sources(
        files, root=root, clang_format=clang_format, check=check, mirror=mirror, verbose=verbose,
    )
    offenders = violating_files(result, root) if (check and not result.ok) else []
    return FormatResult(
        ok=result.ok,
        check=check,
        dirty_only=dirty_only,
        files=len(files),
        duration_s=result.duration_s,
        stderr_log=result.stderr_log,
        offenders=offenders,
    )


def violating_files(result: StepResult, root: Path) -> list[Path]:
    """Parse a check-mode StepResult's captured output for the files clang-format
    flagged as non-conforming, as paths relative to `root` where possible."""
    try:
        text = result.stderr_log.read_text(encoding="utf-8", errors="replace")
    except OSError:
        text = ""
    seen: list[Path] = []
    for m in _VIOLATION_RE.finditer(text):
        p = Path(m.group("file"))
        if p not in seen:
            seen.append(p)
    return seen
