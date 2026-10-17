"""The files an extra root contributes to a check: listing, gitignore-style filtering, and scoped change sets.

An extra root (core/layout.py's `ExtraRoot`) is a directory outside the first-party roots that some checks still own.
A submodule is the usual case, and the reason this module exists: the superproject lists and diffs it as one gitlink, so its files come from its own git.
"""

from __future__ import annotations

import re
import subprocess
import sys
from functools import lru_cache
from pathlib import Path

from ..core import console, profile
from ..core.layout import ExtraRoot
from .changes import ChangeScope, ChangeScopeError, changed_files, submodule_changes

# Warnings already printed, so a lint run asking for files and then for line ranges says each once.
_warned: set[str] = set()


@lru_cache(maxsize=None)
def _glob_regex(pattern: str) -> re.Pattern[str]:
    """A gitignore-style glob as a regex over a root-relative POSIX path.

    A glob without a `/` matches a name at any depth, and a leading `/` anchors one that has none.
    `**` crosses directories, `*` and `?` do not; `glob_matches` handles a glob naming a directory.
    """
    anchored = pattern.startswith("/")
    body = pattern.strip("/")
    if not anchored and "/" not in body:
        body = "**/" + body

    out = []
    i = 0
    while i < len(body):
        if body.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif body.startswith("**", i):
            out.append(".*")
            i += 2
        elif body[i] == "*":
            out.append("[^/]*")
            i += 1
        elif body[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(body[i]))
            i += 1
    return re.compile("".join(out))


def glob_matches(pattern: str, rel: str) -> bool:
    """Whether `rel`, or a directory above it, matches `pattern` — so `src` and `src/` both take the whole subtree."""
    regex = _glob_regex(pattern)
    parts = rel.split("/")
    return any(regex.fullmatch("/".join(parts[:n])) for n in range(1, len(parts) + 1))


def selects(extra: ExtraRoot, rel: str) -> bool:
    """Whether a path relative to the extra root passes its `include` and `exclude`."""
    if extra.include and not any(glob_matches(p, rel) for p in extra.include):
        return False
    return not any(glob_matches(p, rel) for p in extra.exclude)


def is_checked_out(root: Path, extra: ExtraRoot) -> bool:
    """Whether the extra root is there to read; an uninitialized submodule is an empty directory."""
    base = root / extra.path
    return (base / ".git").exists() if extra.submodule else base.is_dir()


def list_files(root: Path, extra: ExtraRoot, *, pruned: frozenset[str] = frozenset()) -> list[str]:
    """The extra root's files, as paths relative to it: tracked plus untracked-but-not-ignored, filtered.

    `pruned` names directories dropped wherever they sit below the extra root, the way a walk prunes junk.
    """
    if not is_checked_out(root, extra):
        return []
    base = root / extra.path
    argv = ["git", "-C", str(base), "ls-files", "--cached", "--others", "--exclude-standard", "-z"]
    if not extra.submodule:
        # A plain directory is listed by the project's own git, which then names paths from the project root.
        argv = ["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard", "-z", "--", extra.path]
    with profile.span(f"git ls-files {extra.path}", type="git"):
        out = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace", check=True)

    prefix = "" if extra.submodule else extra.path + "/"
    found: list[str] = []
    for name in out.stdout.split("\0"):
        if not name or not name.startswith(prefix):
            continue
        rel = name[len(prefix):]
        if any(part in pruned for part in rel.split("/")[:-1]):
            continue
        if selects(extra, rel) and (base / rel).is_file():
            found.append(rel)
    return found


def scoped_files(
    root: Path, extra: ExtraRoot, scope: ChangeScope, *, pruned: frozenset[str] = frozenset(),
    project_changes: list[Path] | None = None,
) -> tuple[list[Path], dict[Path, list[tuple[int, int]]]]:
    """The extra root's files `scope` covers, as absolute paths, with the line ranges a submodule's diff yields.

    A plain directory takes its share of the project's change set, whose line ranges the project's own diff already has, so it yields none.
    `project_changes` is that change set when the caller has it already.
    """
    if not is_checked_out(root, extra):
        return [], {}
    base = (root / extra.path).resolve()
    if extra.submodule:
        try:
            files, ranges = submodule_changes(root, extra.path, scope)
        except ChangeScopeError as e:
            # A gitlink the submodule has not fetched; the project's own files are still worth checking, so this warns rather than fails.
            message = f"WARNING: {extra.path} is not checked in this scope: {e}"
            if message not in _warned:
                _warned.add(message)
                print(console.yellow(message), file=sys.stderr)
            return [], {}
    else:
        files, ranges = (project_changes if project_changes is not None else changed_files(root, scope)), {}

    kept: list[Path] = []
    for p in files:
        p = p.resolve()
        if not p.is_relative_to(base):
            continue
        rel = p.relative_to(base).as_posix()
        if any(part in pruned for part in rel.split("/")[:-1]) or not selects(extra, rel):
            continue
        kept.append(p)
    kept_set = set(kept)
    return kept, {p: r for p, r in ranges.items() if p in kept_set}
