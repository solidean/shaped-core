"""Pick what to measure, and whose flags to measure it with.

A header has no compile command of its own, so one is borrowed from a TU of the target that owns it — "owns" meaning the target with the longest source-directory prefix in common with the header.
That is what makes `clean-core/container/vector.hh` compile with clean-core's include paths and defines rather than some unrelated target's.
A source file is simpler: its own entry is in the database.

Public API:
    resolve_headers(patterns, entries, root, ...) -> list[(Path, entry, target)]
    resolve_sources(patterns, entries, root, ...) -> list[(Path, entry, target)]
"""

from __future__ import annotations

import fnmatch
import os
from pathlib import Path

# Suffixes that count as a header when a pattern matches a directory rather than files.
HEADER_SUFFIXES = (".hh", ".h", ".hpp", ".hxx")
SOURCE_SUFFIXES = (".cc", ".cpp", ".cxx", ".c")


def _norm(p: str | Path) -> str:
    return os.path.normcase(os.path.normpath(str(p))).replace(os.sep, "/")


def target_of(entry: dict) -> str:
    """The CMake target an entry belongs to, read out of its object path.

    CMake writes objects under `CMakeFiles/<target>.dir/`, which is the only place the database names the target at all.
    """
    parts = Path(entry.get("output", "")).as_posix().split("/")
    for i, part in enumerate(parts):
        if part == "CMakeFiles" and i + 1 < len(parts) and parts[i + 1].endswith(".dir"):
            return parts[i + 1][:-4]
    return ""


def expand(patterns: list[str], root: Path, suffixes: tuple[str, ...]) -> list[Path]:
    """Files matching the given glob patterns, resolved against the repo root and the CWD.

    Globs are expanded here rather than by the shell: PowerShell hands wildcards to a native command verbatim.
    A pattern naming a directory expands to every matching file beneath it.
    """
    found: list[Path] = []
    seen: set[str] = set()
    # Comma-lists as well as repetition, matching how --preset and --target are selected everywhere else.
    for raw in [p for spec in patterns for p in spec.split(",") if p.strip()]:
        pattern = raw.strip().replace("\\", "/")
        candidates: list[Path] = []
        for base in (Path.cwd(), root):
            probe = (base / pattern) if not Path(pattern).is_absolute() else Path(pattern)
            if probe.is_dir():
                candidates = [p for p in probe.rglob("*") if p.suffix in suffixes]
                break
            if any(c in pattern for c in "*?["):
                candidates = [p for p in base.glob(pattern) if p.suffix in suffixes]
                if candidates:
                    break
            elif probe.is_file():
                candidates = [probe]
                break
        for path in sorted(candidates):
            key = _norm(path)
            if key not in seen:
                seen.add(key)
                found.append(path)
    return found


def _entries_by_target(entries: list[dict]) -> dict[str, list[dict]]:
    by: dict[str, list[dict]] = {}
    for entry in entries:
        by.setdefault(target_of(entry), []).append(entry)
    return by


def _common_prefix_len(a: str, b: str) -> int:
    """How many leading path segments `a` and `b` share."""
    pa, pb = a.split("/"), b.split("/")
    n = 0
    while n < len(pa) and n < len(pb) and pa[n] == pb[n]:
        n += 1
    return n


def _pick_donor(header: Path, entries: list[dict], targets: list[str] | None) -> tuple[dict, str] | None:
    """The compile entry whose target most plausibly owns `header`.

    Scored by shared path prefix against each candidate TU, so the winner is a TU sitting next to the header in the source tree.
    Ties go to the shortest command, which prefers a library TU over a test TU carrying extra include paths.
    """
    want = _norm(header)
    best: tuple[int, int, dict, str] | None = None
    for entry in entries:
        target = target_of(entry)
        if targets and not any(fnmatch.fnmatch(target, t) for t in targets):
            continue
        score = _common_prefix_len(want, _norm(entry["file"]))
        key = (score, -len(entry["command"]))
        if best is None or key > (best[0], best[1]):
            best = (score, -len(entry["command"]), entry, target)
    if best is None or best[0] == 0:
        return None
    return best[2], best[3]


def resolve_headers(
    patterns: list[str], entries: list[dict], root: Path, *, targets: list[str] | None = None,
) -> list[tuple[Path, dict, str]]:
    """Match header globs, pairing each with a borrowed compile entry and its target.

    Headers with no plausible donor are dropped: measuring one with unrelated flags produces a number that means nothing.
    """
    out: list[tuple[Path, dict, str]] = []
    for header in expand(patterns, root, HEADER_SUFFIXES):
        picked = _pick_donor(header, entries, targets)
        if picked is not None:
            out.append((header, picked[0], picked[1]))
    return out


def resolve_sources(
    patterns: list[str], entries: list[dict], root: Path, *, targets: list[str] | None = None,
) -> list[tuple[Path, dict, str]]:
    """Match source globs against the compilation database, keeping only files it actually knows how to build."""
    by_file = {_norm(e["file"]): e for e in entries}
    out: list[tuple[Path, dict, str]] = []
    for source in expand(patterns, root, SOURCE_SUFFIXES):
        entry = by_file.get(_norm(source))
        if entry is None:
            continue
        target = target_of(entry)
        if targets and not any(fnmatch.fnmatch(target, t) for t in targets):
            continue
        out.append((source, entry, target))
    return out


def object_targets(sources: list[Path], entries: list[dict], build_dir: Path,
                   targets: list[str] | None = None) -> list[str]:
    """Ninja target names for the objects those sources compile to.

    Build-dir-relative and forward-slashed, because that is how they appear in build.ninja — an absolute path is rejected as an unknown target.
    """
    by_file = {_norm(e["file"]): e for e in entries}
    out: list[str] = []
    for source in sources:
        entry = by_file.get(_norm(source))
        if entry is None or not entry.get("output"):
            continue
        if targets and not any(fnmatch.fnmatch(target_of(entry), t) for t in targets):
            continue
        out.append(os.path.relpath(entry["output"], build_dir).replace(os.sep, "/"))
    return out
