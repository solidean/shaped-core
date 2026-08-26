"""Net-diff line space: the thing every change must account for.

A review's obligation is the set of **atoms** the branch produced against its pinned base.
An atom is one added line, one removed line, or one file-level fact that no line can express.

Added lines are keyed by their post-image number under the head path; removed lines by their pre-image number under the base path.
That is what makes a removed line addressable at all, and under a rename the two sides carry different paths, which is correct.

Atoms come from a `--unified=0` diff and only from there.
A wider context is what a human reads, but context lines are not changes, so letting them into the atom set would inflate every denominator.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable

from ..git.diffparse import FileDiff, parse
from ..git.run import Git
from .intervals import IntervalList

ADDED = "+"
REMOVED = "-"


@dataclass(frozen=True)
class FileAtom:
    """A file-level change with no line to point at."""

    path: str
    kind: str
    discriminant: str = ""

    def describe(self) -> str:
        return f"{self.path} [{self.kind}{': ' + self.discriminant if self.discriminant else ''}]"


@dataclass
class LineSpace:
    """A set of atoms, held as intervals per (side, path) plus a set of file atoms."""

    lines: dict[tuple[str, str], IntervalList] = field(default_factory=dict)
    files: frozenset[FileAtom] = frozenset()

    @staticmethod
    def empty() -> LineSpace:
        return LineSpace({}, frozenset())

    def __len__(self) -> int:
        return sum(len(v) for v in self.lines.values()) + len(self.files)

    @property
    def is_empty(self) -> bool:
        return len(self) == 0

    def add(self, side: str, path: str, spans: IntervalList) -> None:
        if not spans:
            return
        key = (side, path)
        self.lines[key] = self.lines[key].union(spans) if key in self.lines else spans

    def get(self, side: str, path: str) -> IntervalList:
        return self.lines.get((side, path), IntervalList())

    def union(self, other: LineSpace) -> LineSpace:
        out = LineSpace({k: v for k, v in self.lines.items()}, self.files | other.files)
        for key, spans in other.lines.items():
            out.lines[key] = out.lines[key].union(spans) if key in out.lines else spans
        return out

    def intersect(self, other: LineSpace) -> LineSpace:
        lines = {}
        for key, spans in self.lines.items():
            hit = spans.intersect(other.lines.get(key, IntervalList()))
            if hit:
                lines[key] = hit
        return LineSpace(lines, self.files & other.files)

    def subtract(self, other: LineSpace) -> LineSpace:
        lines = {}
        for key, spans in self.lines.items():
            rest = spans.subtract(other.lines.get(key, IntervalList()))
            if rest:
                lines[key] = rest
        return LineSpace(lines, self.files - other.files)

    def paths(self) -> list[str]:
        seen = {path for _, path in self.lines} | {a.path for a in self.files}
        return sorted(seen)

    def for_path(self, path: str) -> LineSpace:
        lines = {k: v for k, v in self.lines.items() if k[1] == path}
        return LineSpace(lines, frozenset(a for a in self.files if a.path == path))

    def runs(self) -> list[tuple[str, str, int, int]]:
        """(path, side, start, end) for every contiguous run, in a stable reading order."""
        out = []
        for (side, path), spans in self.lines.items():
            for start, end in spans:
                out.append((path, side, start, end))
        return sorted(out)

    def to_records(self) -> dict:
        """The JSON shape a ledger row stores a claim in."""
        return {
            "lines": {f"{side}\t{path}": [[a, b] for a, b in spans] for (side, path), spans in sorted(self.lines.items())},
            "files": sorted([a.path, a.kind, a.discriminant] for a in self.files),
        }

    @staticmethod
    def from_records(raw: dict) -> LineSpace:
        lines = {}
        for key, spans in (raw.get("lines") or {}).items():
            side, _, path = key.partition("\t")
            lines[(side, path)] = IntervalList.of((int(a), int(b)) for a, b in spans)
        files = frozenset(FileAtom(p, k, d) for p, k, d in (raw.get("files") or []))
        return LineSpace(lines, files)


def space_of(diffs: Iterable[FileDiff]) -> LineSpace:
    """The atom set a parsed `--unified=0` diff describes."""
    space = LineSpace.empty()
    files: set[FileAtom] = set()
    for f in diffs:
        for hunk in f.hunks:
            added, removed = hunk.sides()
            space.add(ADDED, f.new_path or f.path, IntervalList.from_numbers(added))
            space.add(REMOVED, f.old_path or f.path, IntervalList.from_numbers(removed))
        for kind, discriminant in f.file_atoms():
            files.add(FileAtom(f.path, kind, discriminant))
    space.files = frozenset(files)
    return space


def build(git: Git, base: str, head: str) -> LineSpace:
    """The review's whole obligation, from the pinned base to the pinned head."""
    return space_of(parse(git.diff(base, head, context=0)))
