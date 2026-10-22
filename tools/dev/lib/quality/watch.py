"""What a gate or a test watches: globs over repo-relative paths, and whether a change touches them.

A `check` gate declares the files it is about, and a thorough test declares the files it reruns on, both as globs against repo-relative POSIX paths.
The same matcher serves both, so the two never disagree on what `**` means.

Grammar, gitignore-like rather than `fnmatch`:

- `**/` matches any number of directories, including none; a trailing `/**` is everything under a directory.
- `*` and `?` never cross a `/`.
- A pattern without any wildcard names one file, or one directory and everything under it.

A glob that matches no tracked file is an error, never a silently disarmed rule.
The file it was written for has moved, and the gate or test it guards would otherwise stop running without anyone noticing.
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from pathlib import Path

from ..core import profile


def _translate(pattern: str) -> re.Pattern[str]:
    out = ""
    i = 0
    n = len(pattern)
    while i < n:
        c = pattern[i]
        if pattern.startswith("**/", i):
            out += "(?:.*/)?"
            i += 3
        elif pattern.startswith("**", i):
            out += ".*"
            i += 2
        elif c == "*":
            out += "[^/]*"
            i += 1
        elif c == "?":
            out += "[^/]"
            i += 1
        else:
            out += re.escape(c)
            i += 1
    if not any(ch in pattern for ch in "*?"):
        # A bare path is the file itself, or the directory and its whole tree.
        out += "(?:/.*)?"
    return re.compile(f"^{out}$")


@dataclass(frozen=True)
class Watch:
    """A compiled set of globs, kept with their source text for messages."""

    patterns: tuple[str, ...]
    _compiled: tuple[re.Pattern[str], ...]

    @staticmethod
    def of(patterns: tuple[str, ...] | list[str]) -> Watch:
        pats = tuple(p.strip("/") for p in patterns)
        return Watch(pats, tuple(_translate(p) for p in pats))

    def matches(self, rel_posix: str) -> bool:
        return any(rx.match(rel_posix) for rx in self._compiled)

    def touched_by(self, paths: list[str]) -> list[str]:
        """The subset of `paths` this watch covers."""
        return [p for p in paths if self.matches(p)]

    def dead_patterns(self, tracked: list[str]) -> list[str]:
        """The patterns that match none of `tracked`, the rules a moved file has disarmed."""
        return [p for p, rx in zip(self.patterns, self._compiled) if not any(rx.match(t) for t in tracked)]


def tracked_files(root: Path) -> list[str]:
    """Every file git tracks under `root`, as repo-relative POSIX paths; empty when git cannot answer."""
    try:
        with profile.span("git ls-files", type="git"):
            out = subprocess.run(
                ["git", "ls-files", "-z"],
                cwd=str(root), capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60,
            )
    except (OSError, subprocess.TimeoutExpired):
        return []
    if out.returncode != 0:
        return []
    return [entry for entry in out.stdout.split("\0") if entry]
