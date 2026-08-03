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

# Default if `.clang-format`'s `Requires:` header can't be read.
# Keep in sync
# with the header, which remains the authoritative source.
_DEFAULT_MAJOR = 22

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
    the common LLVM install locations.
    Returns None if nothing usable is found."""
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
    sync.
    Falls back to _DEFAULT_MAJOR if the header is missing or unreadable.
    """
    try:
        text = (root / ".clang-format").read_text(encoding="utf-8")
    except OSError:
        return _DEFAULT_MAJOR
    m = re.search(r"Requires:\s*clang-format\s*>=\s*(\d+)", text)
    return int(m.group(1)) if m else _DEFAULT_MAJOR


def _git_dirty_files(root: Path) -> list[Path]:
    """Files that are git-dirty or untracked — what's reasonably part of the next
    commit.
    Deletions are dropped (nothing to format); renames yield their new
    path.
    Returns absolute paths; nonexistent entries are filtered out."""
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


# A whole untracked file is "changed", and its length is not worth a stat — the linter clamps anyway.
_ALL_LINES = 0xFFFFFFFF


def changed_line_ranges(root: Path) -> dict[Path, list[tuple[int, int]]]:
    """The 1-based line ranges each dirty file changed, as absolute paths -> [(first, last), ...].

    This is what makes a dirty-only prose run line-exact instead of file-wide.
    A prose finding sits on one line, so it either changed or it did not; a code finding can be caused by
    a line the edit never touched, which is why only prose rules are scoped this way.

    Tracked changes come from `git diff -U0 HEAD`, so staged and unstaged both count.
    An untracked file has no diff and is reported as changed end to end.
    A pure-deletion hunk marks the surviving line above it, which is the one the edit could have broken.
    """
    ranges: dict[Path, list[tuple[int, int]]] = {}

    try:
        out = subprocess.run(
            ["git", "diff", "--unified=0", "--no-color", "HEAD"],
            cwd=str(root), capture_output=True, text=True, timeout=60,
        )
    except (OSError, subprocess.TimeoutExpired):
        out = None

    if out is not None and out.returncode == 0:
        current: Path | None = None
        for line in out.stdout.splitlines():
            if line.startswith("+++ "):
                target = line[4:].strip()
                if target == "/dev/null":
                    current = None
                else:
                    current = (root / (target[2:] if target.startswith("b/") else target)).resolve()
                continue
            if current is None or not line.startswith("@@"):
                continue

            m = re.match(r"@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", line)
            if not m:
                continue
            start = int(m.group(1))
            count = int(m.group(2)) if m.group(2) is not None else 1
            if count == 0:
                ranges.setdefault(current, []).append((max(1, start), max(1, start)))
            else:
                ranges.setdefault(current, []).append((start, start + count - 1))

    try:
        untracked = subprocess.run(
            ["git", "ls-files", "--others", "--exclude-standard"],
            cwd=str(root), capture_output=True, text=True, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired):
        untracked = None

    if untracked is not None and untracked.returncode == 0:
        for name in untracked.stdout.splitlines():
            if not name.strip():
                continue
            p = (root / name.strip()).resolve()
            if p.is_file():
                ranges.setdefault(p, []).append((1, _ALL_LINES))

    return ranges


def format_changed_line_spec(ranges: dict[Path, list[tuple[int, int]]]) -> str:
    """Render `changed_line_ranges` as shaped-linter's `--changed-lines` spec: `<path>:a-b,c-d` per line."""
    lines = []
    for path in sorted(ranges):
        spec = ",".join(f"{first}-{last}" for first, last in ranges[path])
        lines.append(f"{path}:{spec}")
    return "\n".join(lines) + ("\n" if lines else "")


def source_roots(root: Path) -> list[Path]:
    """The directories whose `.cc`/`.hh` files clang-format owns.

    A whitelist, not a repo-wide sweep: extern/ is vendored third-party code we must not reformat,
    and a stray source elsewhere should not silently become our problem.
    Add a root here when the
    repo grows first-party C++ outside libs/.
    """
    return [
        root / "libs",
        root / "tools" / "instruction-tracer",
        root / "tools" / "shaped-linter",
    ]


def discover_files(root: Path, *, dirty_only: bool) -> list[Path]:
    """Return the sorted list of `.cc`/`.hh` files under the source roots to format.

    With `dirty_only`, restrict to git-dirty/untracked files (intersected with the same scope).
    """
    roots = [r for r in source_roots(root) if r.is_dir()]

    if dirty_only:
        selected = [
            p for p in _git_dirty_files(root)
            if p.suffix in _SOURCE_SUFFIXES and any(r in p.parents for r in roots)
        ]
        return sorted(set(selected))

    found: list[Path] = []
    for source_root in roots:
        for dirpath, _dirnames, filenames in os.walk(source_root):
            for f in filenames:
                if f.endswith(_SOURCE_SUFFIXES):
                    found.append(Path(dirpath) / f)
    return sorted(found)


# shaped-linter's scope, which is wider than clang-format's in both axes.
#
# It lints prose as well as code, so markdown and Python are in — and prose lives in docs/ and in the
# skill files as much as in libs/. Kept separate from the clang-format scope above rather than widening
# it: clang-format must never be pointed at a .md, and the two are answering different questions.
_LINT_SUFFIXES = (".cc", ".hh", ".md", ".py")

_LINT_ROOTS = ("libs", "tools", "docs", ".claude/skills")

# First-party files at the repo root, which is not a directory we can walk wholesale.
_LINT_ROOT_FILES = ("CLAUDE.md", "readme.md", "dev.py")

# Never ours to lint: vendored code, build output, and caches.
_LINT_EXCLUDED_DIRS = frozenset({"extern", "build", ".venv", "__pycache__", "node_modules", ".git"})


def lint_roots(root: Path) -> list[Path]:
    """The directories shaped-linter owns, as absolute paths."""
    return [root / r for r in _LINT_ROOTS]


def _is_lintable(path: Path, roots: list[Path], root: Path) -> bool:
    if path.suffix not in _LINT_SUFFIXES:
        return False
    if any(part in _LINT_EXCLUDED_DIRS for part in path.parts):
        return False
    if any(r in path.parents for r in roots):
        return True
    return path.parent == root and path.name in _LINT_ROOT_FILES


def discover_lint_files(root: Path, *, dirty_only: bool) -> list[Path]:
    """Return the sorted list of files shaped-linter should lint.

    Wider than `discover_files`: it covers `.md` and `.py` too, and reaches docs/ and .claude/skills/,
    because the linter's prose rules bind every file a human writes sentences in.
    With `dirty_only`, restrict to git-dirty/untracked files (intersected with the same scope).
    """
    roots = [r for r in lint_roots(root) if r.is_dir()]

    if dirty_only:
        return sorted({p for p in _git_dirty_files(root) if _is_lintable(p, roots, root)})

    found: list[Path] = []
    for lint_root in roots:
        for dirpath, dirnames, filenames in os.walk(lint_root):
            dirnames[:] = [d for d in dirnames if d not in _LINT_EXCLUDED_DIRS]
            for f in filenames:
                if f.endswith(_LINT_SUFFIXES):
                    found.append(Path(dirpath) / f)
    found += [root / f for f in _LINT_ROOT_FILES if (root / f).is_file()]
    return sorted(found)


def expand_lint_paths(root: Path, paths: list[str]) -> list[Path]:
    """Expand user-given files and directories into the lintable files under them.

    Same suffix and exclusion rules as `discover_lint_files`, so `prose-stats docs/` measures exactly the
    files `lint shaped` would have walked there.
    A named file is taken as given — measuring one deliberately is not the same as discovering it.
    """
    found: list[Path] = []
    for raw in paths:
        p = Path(raw)
        if not p.is_absolute():
            p = root / p

        if p.is_file():
            found.append(p)
            continue

        for dirpath, dirnames, filenames in os.walk(p):
            dirnames[:] = [d for d in dirnames if d not in _LINT_EXCLUDED_DIRS]
            for f in filenames:
                if f.endswith(_LINT_SUFFIXES):
                    found.append(Path(dirpath) / f)

    return sorted(set(found))


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
            f"clang-format not found on PATH. Install LLVM/clang-format (>= {required_major(root)}) "
            "or add it to PATH."
        )

    # clang-format output is not stable across major versions, so enforce the
    # major declared by .clang-format.
    # allow_different_version downgrades the
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
