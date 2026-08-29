"""clang-format integration: locate clang-format, check its version, and run it.

This backs `dev.py format`.
clang-format output is not stable across major versions, so the tooling pins to the major declared by `.clang-format`'s `Requires: clang-format >= N` header — the same file that defines the style.
`format_sources` runs a single clang-format invocation through the shared `run_step`, either rewriting files in place or, in check mode, reporting which files do not conform.
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
from .changes import ChangeScope, changed_files


class FormatSetupError(Exception):
    """Unrecoverable setup problem for a format run — clang-format missing, or a version mismatch — carrying the message the CLI should surface."""


@dataclass(frozen=True)
class FormatResult:
    """Outcome of a `run_format` invocation, for the CLI to present.

    `nothing` flags "no files in scope" (a success). In check mode `offenders`
    lists the non-conforming files when `ok` is False.
    `scope` is the change set the run was restricted to, or None for the whole tree, and the summary wording follows it.
    """

    ok: bool
    check: bool
    scope: ChangeScope | None
    files: int
    duration_s: float = 0.0
    nothing: bool = False
    stderr_log: Path | None = None
    offenders: list[Path] = field(default_factory=list)

# Default if `.clang-format`'s `Requires:` header cannot be read.
# Keep in sync with the header, which remains the authoritative source.
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
# The level is "warning" without -Werror and "error" with it.
# The leading path can itself contain colons on Windows ("C:\..."), so the non-greedy capture stops at the first ":<line>:<col>:", which is the real separator.
_VIOLATION_RE = re.compile(r"^(?P<file>.+?):\d+:\d+:\s+(?:error|warning):", re.MULTILINE)


def repo_clang_format(root: Path) -> Path:
    """Where tools/bin/fetch-clang-format.py installs the pinned clang-format, whether or not it is there yet."""
    return root / "tools" / "bin" / ("clang-format.exe" if os.name == "nt" else "clang-format")


def find_clang_format(explicit: str | None = None, root: Path | None = None) -> str | None:
    """Locate the clang-format executable: an explicit path or name, then the repo-local pinned one, then PATH, then the common LLVM install locations.

    The repo-local copy outranks PATH because it is the only one whose version we chose — see ensure_pinned_clang_format.
    It exists only once something fetched it, so a machine whose PATH clang-format is already the right major never grows one.
    None when nothing usable is found.
    """
    if explicit:
        if Path(explicit).is_file():
            return explicit
        return shutil.which(explicit)
    if root is not None:
        pinned = repo_clang_format(root)
        if pinned.is_file():
            return str(pinned)
    found = shutil.which("clang-format")
    if found:
        return found
    for candidate in _FALLBACK_PATHS:
        if candidate.is_file():
            return str(candidate)
    return None


def ensure_pinned_clang_format(root: Path) -> str | None:
    """Fetch the pinned clang-format into tools/bin and return its path, or None if that could not be done.

    Called only once the resolved clang-format is missing or the wrong major, so the common case never runs it.
    Set SC_SKIP_CLANG_FORMAT_FETCH=1 to keep it from reaching the network, which then leaves the version error to be reported as before.
    """
    if os.environ.get("SC_SKIP_CLANG_FORMAT_FETCH"):
        return None
    script = root / "tools" / "bin" / "fetch-clang-format.py"
    if not script.is_file():
        return None
    try:
        completed = subprocess.run(
            [sys.executable, str(script)], cwd=str(root), capture_output=True, text=True, timeout=300
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    # The script narrates itself on stderr, success or failure — a download worth seconds, or why it could not be done.
    # Passed through either way, so the pause is never unexplained and a failure is not restated in worse words.
    if completed.stderr.strip():
        print(completed.stderr.strip(), file=sys.stderr)
    if completed.returncode != 0:
        return None
    pinned = repo_clang_format(root)
    return str(pinned) if pinned.is_file() else None


def clang_format_version(exe: str) -> tuple[int, ...] | None:
    """clang-format's version as a tuple, e.g. (21, 1, 0), or None when it cannot be run or parsed."""
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

    Parses the `Requires: clang-format >= N` header, so the version check enforces exactly what the style file declares with no second constant to keep in sync.
    Falls back to _DEFAULT_MAJOR when the header is missing or unreadable.
    """
    try:
        text = (root / ".clang-format").read_text(encoding="utf-8")
    except OSError:
        return _DEFAULT_MAJOR
    m = re.search(r"Requires:\s*clang-format\s*>=\s*(\d+)", text)
    return int(m.group(1)) if m else _DEFAULT_MAJOR


def source_roots(root: Path) -> list[Path]:
    """The directories whose `.cc`/`.hh` files clang-format owns.

    A whitelist rather than a repo-wide sweep: extern/ is vendored third-party code we must not reformat, and a stray source elsewhere should not silently become our problem.
    Add a root here when the repo grows first-party C++ outside libs/.
    """
    return [
        root / "libs",
        root / "tools" / "instruction-tracer",
        root / "tools" / "shaped-linter",
    ]


def discover_files(root: Path, *, scope: ChangeScope | None) -> list[Path]:
    """Return the sorted list of `.cc`/`.hh` files under the source roots to format.

    A `scope` restricts the set to what that change set touched (see quality/changes.py); None means the whole tree.
    """
    roots = [r for r in source_roots(root) if r.is_dir()]

    if scope is not None:
        selected = [
            p for p in changed_files(root, scope)
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
_LINT_ROOT_FILES = ("CLAUDE.md", "README.md", "dev.py")

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


def discover_lint_files(root: Path, *, scope: ChangeScope | None) -> list[Path]:
    """Return the sorted list of files shaped-linter should lint.

    Wider than `discover_files`: it covers `.md` and `.py` too, and reaches docs/ and .claude/skills/,
    because the linter's prose rules bind every file a human writes sentences in.
    A `scope` restricts the set to what that change set touched (see quality/changes.py); None means the whole tree.
    """
    roots = [r for r in lint_roots(root) if r.is_dir()]

    if scope is not None:
        return sorted({p for p in changed_files(root, scope) if _is_lintable(p, roots, root)})

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

    In apply mode it rewrites files in place (`-i`).
    In check mode it runs `--dry-run -Werror`, which exits non-zero and names each non-conforming file in its output without modifying anything.
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
    scope: ChangeScope | None,
    allow_different_version: bool,
    mirror: bool = False,
    verbose: bool = False,
) -> FormatResult:
    """Locate clang-format, enforce its version, and format the selected sources.

    Raises FormatSetupError for an unrecoverable setup problem: clang-format missing, its version undeterminable, or a major mismatch.
    `allow_different_version` downgrades that last one to a yellow warning instead.
    Returns a FormatResult describing what happened, including the "no files in scope" success; the caller prints the summary (see report.summarize_format).
    """
    need = required_major(root)
    clang_format = find_clang_format(root=root)
    have = clang_format_version(clang_format) if clang_format is not None else None

    # clang-format output is not stable across major versions, so enforce the major declared by .clang-format.
    # Nothing usable means fetch the pinned build rather than send the caller off to install LLVM by hand — that is
    # what tools/bin/fetch-clang-format.py is for, and it is a ~1.5 MB download that happens once per machine.
    if clang_format is None or have is None or have[0] != need:
        fetched = ensure_pinned_clang_format(root)
        if fetched is not None:
            clang_format = fetched
            have = clang_format_version(clang_format)

    if clang_format is None:
        raise FormatSetupError(
            f"clang-format not found, and the pinned {need}.x could not be fetched into tools/bin. "
            "Install LLVM/clang-format or add it to PATH."
        )
    if have is None:
        raise FormatSetupError(f"could not determine clang-format version from {clang_format!r}")

    # allow_different_version downgrades a surviving mismatch to a warning instead of failing.
    if have[0] != need:
        have_str = ".".join(str(p) for p in have)
        msg = (f"clang-format major version {have[0]} != required {need} "
               f"(found {have_str}); formatting may differ from the pinned style")
        if allow_different_version:
            print(console.yellow(f"WARNING: {msg}"), file=sys.stderr)
        else:
            raise FormatSetupError(
                f"{msg}. Run tools/bin/fetch-clang-format.py to install the pinned {need}.x, "
                "or pass --allow-different-version to proceed anyway."
            )

    files = discover_files(root, scope=scope)
    if not files:
        return FormatResult(ok=True, check=check, scope=scope, files=0, nothing=True)

    result = format_sources(
        files, root=root, clang_format=clang_format, check=check, mirror=mirror, verbose=verbose,
    )
    offenders = violating_files(result, root) if (check and not result.ok) else []
    return FormatResult(
        ok=result.ok,
        check=check,
        scope=scope,
        files=len(files),
        duration_s=result.duration_s,
        stderr_log=result.stderr_log,
        offenders=offenders,
    )


def violating_files(result: StepResult, root: Path) -> list[Path]:
    """Parse a check-mode StepResult's captured output for the files clang-format flagged as non-conforming, as paths relative to `root` where possible."""
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
