"""Mapping a commit's own line numbers onto the review's obligation.

A commit-local hunk is numbered in *that commit's* tree, and the review accounts for the net diff against the pinned base.
So a hunk from commit C claims nothing until its lines are carried to where the ledger speaks: added lines forward to head,
removed lines back to base.

Both directions are one diff each, not a walk over the commits between.
`git diff -U0 C head` already states which regions of C differ from head, and the regions it leaves out are unchanged —
which is exactly the line mapping, composed, for free.

A line inside a region a later commit rewrote is **killed** rather than mapped.
That is the conservative reading and the correct one: the later commit owns that line now, so C's hunk does not answer for it.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from ..git.diffparse import parse
from ..git.run import Git
from .intervals import IntervalList

# A line that did not survive; distinguishable from a real line number, which is always >= 1.
KILLED = 0


@dataclass(frozen=True)
class Edit:
    """One region that differs between two trees, in both sides' numbering."""

    old_start: int
    old_len: int
    new_start: int
    new_len: int


@dataclass
class FileMap:
    """The line mapping for one file between two trees."""

    edits: list[Edit] = field(default_factory=list)
    renamed_to: str = ""

    def forward(self, line: int) -> int:
        """Where `line` sits in the later tree, or KILLED when that tree no longer has it verbatim."""
        delta = 0
        for edit in self.edits:
            if edit.old_start > line:
                break
            if line < edit.old_start + edit.old_len:
                return KILLED
            delta += edit.new_len - edit.old_len
        return line + delta

    def backward(self, line: int) -> int:
        """The inverse: where a line of the later tree sat in the earlier one."""
        delta = 0
        for edit in self.edits:
            if edit.new_start > line:
                break
            if line < edit.new_start + edit.new_len:
                return KILLED
            delta += edit.old_len - edit.new_len
        return line + delta


@dataclass
class TreeMap:
    """Line mappings for every file that differs between two commits."""

    files: dict[str, FileMap] = field(default_factory=dict)

    def of(self, path: str) -> FileMap:
        """An untouched file maps identically, which is what the empty FileMap does."""
        return self.files.get(path, FileMap())

    def target_path(self, path: str) -> str:
        return self.files.get(path, FileMap()).renamed_to or path


def build(git: Git, earlier: str, later: str) -> TreeMap:
    """The mapping between two commits, from one `--unified=0` diff."""
    tree = TreeMap()
    for file in parse(git.diff(earlier, later, context=0)):
        old_path = file.old_path or file.path
        new_path = file.new_path or file.path
        entry = FileMap(renamed_to=new_path if new_path != old_path else "")
        for hunk in file.hunks:
            # A zero-length side is an insertion point, and git numbers it by the line *before* it.
            # Shifting it to `start + 1` makes it a genuinely empty range, so the lookups below need no special case
            # and the boundary line — which did not move — no longer collects the insertion's delta.
            entry.edits.append(Edit(
                old_start=hunk.old_start + 1 if hunk.old_count == 0 else hunk.old_start,
                old_len=hunk.old_count,
                new_start=hunk.new_start + 1 if hunk.new_count == 0 else hunk.new_start,
                new_len=hunk.new_count,
            ))
        entry.edits.sort(key=lambda e: e.old_start)
        tree.files[old_path] = entry
    return tree


def map_forward(tree: TreeMap, path: str, lines: list[int]) -> tuple[str, IntervalList]:
    """(path in the later tree, the lines that survived) for a set of lines in the earlier one."""
    file_map = tree.of(path)
    survived = [mapped for line in lines if (mapped := file_map.forward(line)) != KILLED]
    return tree.target_path(path), IntervalList.from_numbers(survived)


def map_backward(tree: TreeMap, path: str, lines: list[int]) -> IntervalList:
    """The lines of the earlier tree that a set of later-tree lines came from."""
    file_map = tree.of(path)
    survived = [mapped for line in lines if (mapped := file_map.backward(line)) != KILLED]
    return IntervalList.from_numbers(survived)


def source_path(tree: TreeMap, path: str) -> str:
    """The earlier tree's name for a file the later tree calls `path`."""
    for old, file_map in tree.files.items():
        if file_map.renamed_to == path:
            return old
    return path
