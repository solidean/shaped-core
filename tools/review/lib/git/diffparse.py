"""The only module that reads unified-diff syntax.

Everything downstream works on `FileDiff` and `Hunk`, so `@@` parsing has exactly one implementation to get right.
A hunk carries both sides' line numbers, because a removed line is a real change with a real number — in the pre-image.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

_HUNK_RE = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)$")
_DIFF_GIT_RE = re.compile(r"^diff --git (.+)$")

# Which file-level facts get their own change, since none of them is expressible as a line.
FILE_ATOM_KINDS = ("rename", "mode", "binary", "delete", "add")


def unquote_path(raw: str) -> str:
    """Undo git's C-style quoting of a path with unusual bytes, and strip the a/ or b/ prefix."""
    s = raw.strip()
    if s.startswith('"') and s.endswith('"') and len(s) >= 2:
        s = s[1:-1].encode("utf-8", "surrogateescape").decode("unicode_escape")
    if s.startswith(("a/", "b/")):
        s = s[2:]
    return s


def header_paths(rest: str) -> tuple[str, str]:
    """The (old, new) paths from a `diff --git a/X b/Y` line, or ("", "") when they cannot be pinned.

    Git omits the `---` / `+++` lines for a binary file and for a mode-only change, so for those this header
    is the only place the path appears at all — and a file whose change never yields a path is dropped from the diff,
    which is a change with no atom and no id.

    The line is genuinely ambiguous for a path containing a space, which is why this is a fallback rather than the source.
    The usual case pins itself: both sides name the same path, so `len` fixes the split exactly.
    """
    rest = rest.strip()
    if rest.startswith('"'):
        closing = rest.find('" "')
        if closing > 0:
            return unquote_path(rest[:closing + 1]), unquote_path(rest[closing + 2:])
        return "", ""

    # `a/P b/P`: 2 + len(P) + 1 + 2 + len(P) characters, so an odd remainder means the two sides differ.
    if (len(rest) - 5) % 2 == 0:
        width = (len(rest) - 5) // 2
        same = rest[2:2 + width]
        if width > 0 and rest == f"a/{same} b/{same}":
            return same, same

    marker = rest.find(" b/")
    if rest.startswith("a/") and marker > 0:
        return rest[2:marker], rest[marker + 3:]
    return "", ""


@dataclass
class Hunk:
    """One `@@` block, with both sides' numbering and the raw body it was made of."""

    old_start: int
    old_count: int
    new_start: int
    new_count: int
    section: str
    lines: list[str] = field(default_factory=list)

    @property
    def old_end(self) -> int:
        return self.old_start + max(self.old_count, 1) - 1

    @property
    def new_end(self) -> int:
        return self.new_start + max(self.new_count, 1) - 1

    def sides(self) -> tuple[list[int], list[int]]:
        """(added new-side line numbers, removed old-side line numbers), from one walk of the body.

        Two counters seeded by the header: a context line advances both, a `-` advances and emits on the pre-image,
        a `+` advances and emits on the post-image.
        """
        added: list[int] = []
        removed: list[int] = []
        old_no, new_no = self.old_start, self.new_start
        for line in self.lines:
            if not line:
                continue
            marker = line[0]
            if marker == " ":
                old_no += 1
                new_no += 1
            elif marker == "-":
                removed.append(old_no)
                old_no += 1
            elif marker == "+":
                added.append(new_no)
                new_no += 1
            # A `\ No newline at end of file` marker belongs to the line above and advances nothing.
        return added, removed

    def render(self) -> str:
        header = f"@@ -{self.old_start},{self.old_count} +{self.new_start},{self.new_count} @@{self.section}"
        return "\n".join([header, *self.lines])

    def hash_body(self) -> str:
        """The hunk body with its header reduced to a bare `@@`.

        The line numbers and the trailing function context both move when unrelated code above shifts,
        so neither may take part in the identity of the change.
        """
        return "\n".join(["@@", *self.lines])


@dataclass
class FileDiff:
    """One file's entry in a diff, whatever kind of change it is."""

    old_path: str = ""
    new_path: str = ""
    old_mode: str = ""
    new_mode: str = ""
    is_binary: bool = False
    is_new: bool = False
    is_delete: bool = False
    is_rename: bool = False
    hunks: list[Hunk] = field(default_factory=list)
    # What the `diff --git` line said, used only where nothing better set a path.
    header_old: str = ""
    header_new: str = ""

    def fill_paths_from_header(self) -> None:
        """Take the paths from the `diff --git` header when the diff body never gave any.

        Only ever fills what is empty, and respects the add/delete direction:
        the header names both sides even for a file that exists on only one of them.
        """
        if self.old_path or self.new_path:
            return
        if not self.header_old and not self.header_new:
            return
        if not self.is_new:
            self.old_path = self.header_old
        if not self.is_delete:
            self.new_path = self.header_new

    @property
    def path(self) -> str:
        """The path this file is best known by: its post-image path, or its pre-image one when it was deleted."""
        return self.new_path or self.old_path

    def file_atoms(self) -> list[tuple[str, str]]:
        """(kind, discriminant) for what this file changed that no line can express."""
        atoms: list[tuple[str, str]] = []
        if self.is_rename:
            atoms.append(("rename", f"{self.old_path} -> {self.new_path}"))
        if self.old_mode and self.new_mode and self.old_mode != self.new_mode:
            atoms.append(("mode", f"{self.old_mode} -> {self.new_mode}"))
        if self.is_binary:
            kind = "add" if self.is_new else "delete" if self.is_delete else "binary"
            atoms.append(("binary", kind))
        return atoms


def parse(text: str) -> list[FileDiff]:
    """Parse `git diff` output into one FileDiff per file, in the order git emitted them."""
    files: list[FileDiff] = []
    current: FileDiff | None = None
    hunk: Hunk | None = None

    for raw in text.splitlines():
        if raw.startswith("diff --git "):
            current = FileDiff()
            m = _DIFF_GIT_RE.match(raw)
            if m:
                current.header_old, current.header_new = header_paths(m.group(1))
            hunk = None
            files.append(current)
            continue
        if current is None:
            continue

        if raw.startswith("@@"):
            m = _HUNK_RE.match(raw)
            if not m:
                continue
            hunk = Hunk(
                old_start=int(m.group(1)),
                old_count=int(m.group(2)) if m.group(2) is not None else 1,
                new_start=int(m.group(3)),
                new_count=int(m.group(4)) if m.group(4) is not None else 1,
                section=m.group(5),
            )
            current.hunks.append(hunk)
            continue

        if hunk is not None and raw[:1] in (" ", "+", "-", "\\"):
            hunk.lines.append(raw)
            continue

        # Extended headers, which only appear before the first hunk.
        if raw.startswith("--- "):
            target = raw[4:].strip()
            current.old_path = "" if target == "/dev/null" else unquote_path(target)
        elif raw.startswith("+++ "):
            target = raw[4:].strip()
            current.new_path = "" if target == "/dev/null" else unquote_path(target)
        elif raw.startswith("old mode "):
            current.old_mode = raw[len("old mode "):].strip()
        elif raw.startswith("new mode "):
            current.new_mode = raw[len("new mode "):].strip()
        elif raw.startswith("new file mode "):
            current.is_new = True
            current.new_mode = raw[len("new file mode "):].strip()
        elif raw.startswith("deleted file mode "):
            current.is_delete = True
            current.old_mode = raw[len("deleted file mode "):].strip()
        elif raw.startswith("rename from "):
            current.is_rename = True
            current.old_path = unquote_path(raw[len("rename from "):])
        elif raw.startswith("rename to "):
            current.is_rename = True
            current.new_path = unquote_path(raw[len("rename to "):])
        elif raw.startswith("Binary files ") or raw.startswith("GIT binary patch"):
            current.is_binary = True

    for f in files:
        f.fill_paths_from_header()
    return [f for f in files if f.old_path or f.new_path]
