"""Reading an entry file into blocks, and remembering exactly where each one sat.

Every block records its character span in the original text.
That is what lets the writer stamp a round or insert a block as a **splice** rather than a re-serialization,
so a file an agent hand-authored comes back byte-identical everywhere it was not touched.
Re-serializing instead is how a tool ends up reformatting someone's entry and then reporting a spurious immutability failure.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from .grammar import (
    ASK_NAME_RE,
    ATTR_RE,
    BLOCK_TYPES,
    FRONT_KNOWN,
    FRONT_REQUIRED,
    HEADING_RE,
    SEVERITIES,
    SHOW_KINDS,
    STATES,
    Option,
    ReviewParseError,
    did_you_mean,
    parse_option,
)


@dataclass
class Block:
    """One `## <type>` block, with its attributes, prose, options and its span in the file."""

    type: str
    head: str = ""
    attrs: dict[str, str] = field(default_factory=dict)
    prose: str = ""
    options: list[Option] = field(default_factory=list)
    line: int = 0
    start: int = 0
    heading_end: int = 0
    end: int = 0
    raw: str = ""

    @property
    def name(self) -> str:
        """An ask's answer key."""
        return self.head.strip()

    @property
    def change_ids(self) -> list[str]:
        return self.head.split()

    @property
    def discharges(self) -> list[str]:
        return self.attrs.get("discharges", "").split()

    @property
    def round(self) -> int:
        try:
            return int(self.attrs.get("round", "0"))
        except ValueError:
            return 0

    @property
    def is_ask(self) -> bool:
        return self.type == "ask"


@dataclass
class Entry:
    """A parsed entry file."""

    path: Path
    slug: str
    front: dict[str, str] = field(default_factory=dict)
    blocks: list[Block] = field(default_factory=list)
    text: str = ""
    body_start: int = 0
    newline: str = "\n"

    @property
    def id(self) -> str:
        return self.front.get("id", self.slug)

    @property
    def title(self) -> str:
        return self.front.get("title", self.slug)

    @property
    def group(self) -> str:
        return self.front.get("group", "review")

    @property
    def state(self) -> str:
        return self.front.get("state", "open")

    @property
    def severity(self) -> str:
        return self.front.get("severity", "")

    @property
    def asks(self) -> list[Block]:
        return [b for b in self.blocks if b.is_ask]

    def ask(self, name: str) -> Block | None:
        return next((b for b in self.asks if b.name == name), None)

    def referenced_changes(self) -> list[str]:
        """Every change id this entry names, from `changes` heads and `discharges` attributes alike."""
        out: list[str] = []
        for block in self.blocks:
            if block.type == "changes":
                out.extend(block.change_ids)
            out.extend(block.discharges)
        return out

    def discharged_changes(self) -> list[str]:
        """The change ids this entry actually discharges, which only an ask can do."""
        out: list[str] = []
        for block in self.asks:
            out.extend(block.discharges)
        return out


def _split_front(text: str, path: Path) -> tuple[dict[str, str], int, int]:
    """(front matter, character offset the body starts at, line the body starts on)."""
    if not text.startswith("---"):
        return {}, 0, 1

    lines = text.splitlines(keepends=True)
    front: dict[str, str] = {}
    offset = len(lines[0])
    for number, raw in enumerate(lines[1:], start=2):
        offset += len(raw)
        stripped = raw.strip()
        if stripped == "---":
            return front, offset, number + 1
        if not stripped:
            continue
        key, sep, value = stripped.partition(":")
        if not sep:
            raise ReviewParseError(path, number, f"front matter line {stripped!r} is not `key: value`")
        front[key.strip()] = value.strip()
    raise ReviewParseError(path, len(lines), "front matter is never closed",
                           "add a closing `---` line after the last key")


def _validate_front(front: dict[str, str], path: Path) -> None:
    for key in FRONT_REQUIRED:
        if not front.get(key):
            raise ReviewParseError(path, 1, f"front matter is missing `{key}`",
                                   f"add `{key}: ...` between the `---` lines")
    state = front.get("state", "open")
    if state not in STATES:
        raise ReviewParseError(path, 1, f"unknown state {state!r}", f"one of: {', '.join(STATES)}")
    severity = front.get("severity", "")
    if severity and severity not in SEVERITIES:
        raise ReviewParseError(path, 1, f"unknown severity {severity!r}", f"one of: {', '.join(SEVERITIES)}")
    # An unknown front-matter key is preserved rather than rejected, so a review can carry fields the tool has no opinion on.
    _ = FRONT_KNOWN


def _parse_body(block: Block, body: str, first_line: int, path: Path) -> None:
    """Split a block body into its attribute prelude, its options and its prose."""
    lines = body.splitlines()
    allowed = BLOCK_TYPES[block.type]

    consumed = 0
    for raw in lines:
        m = ATTR_RE.match(raw)
        if not m:
            break
        key, value = m.group(1), m.group(2).strip()
        if key not in allowed:
            suggestion = did_you_mean(key, allowed)
            raise ReviewParseError(
                path, first_line + consumed,
                f"`{key}:` is not an attribute of a `{block.type}` block",
                (f"did you mean `{suggestion}:`? " if suggestion else "")
                + "attributes here are: " + (", ".join(sorted(allowed)) or "none")
                + ". To write this as prose, start the block with a blank line.",
            )
        block.attrs[key] = value
        consumed += 1

    prose_lines: list[str] = []
    for raw in lines[consumed:]:
        option = parse_option(raw)
        if option is not None and block.is_ask:
            block.options.append(option)
        else:
            prose_lines.append(raw)
    block.prose = "\n".join(prose_lines).strip("\n")


def _validate_block(block: Block, path: Path, seen_asks: set[str]) -> None:
    if block.is_ask:
        if not block.name:
            raise ReviewParseError(path, block.line, "an `ask` block needs a name",
                                   "write `## ask <name>`; the name is how the answer is keyed")
        if not ASK_NAME_RE.match(block.name):
            raise ReviewParseError(path, block.line, f"{block.name!r} is not a usable ask name",
                                   "lowercase letters, digits and dashes")
        if block.name in seen_asks:
            raise ReviewParseError(path, block.line, f"duplicate ask name {block.name!r} in this entry",
                                   "ask names are the answer key, so they must be unique per entry")
        seen_asks.add(block.name)

        labels = [o.label for o in block.options]
        duplicate = next((label for label in labels if labels.count(label) > 1), "")
        if duplicate:
            raise ReviewParseError(path, block.line, f"duplicate option {duplicate!r} in ask {block.name!r}",
                                   "two options that read the same cannot be told apart in an answer")
    elif block.type == "changes":
        show = block.attrs.get("show", "")
        if not show:
            raise ReviewParseError(
                path, block.line, "a `changes` block must say how it opens",
                "add `show: collapsed` unless the reader cannot decide the entry without the diff, then `show: visible`",
            )
        if show not in SHOW_KINDS:
            raise ReviewParseError(path, block.line, f"unknown show {show!r}", f"one of: {', '.join(SHOW_KINDS)}")
    elif block.head:
        raise ReviewParseError(path, block.line, f"a `{block.type}` block takes no argument, got {block.head!r}",
                               "only `changes` and `ask` take one")


def parse_text(text: str, path: Path, slug: str = "") -> Entry:
    """Parse entry text, raising ReviewParseError with a line number on anything malformed.

    Offsets are computed against LF-normalized text, and the file's own line ending is remembered
    so a write can put it back — a splice that silently converted the whole file would not be a splice.
    """
    newline = "\r\n" if "\r\n" in text else "\n"
    text = text.replace("\r\n", "\n")
    front, body_offset, body_line = _split_front(text, path)
    _validate_front(front, path)

    entry = Entry(path=path, slug=slug or path.stem, front=front, text=text,
                  body_start=body_offset, newline=newline)

    lines = text[body_offset:].splitlines(keepends=True)
    starts: list[tuple[int, int, str]] = []
    offset = body_offset
    for number, raw in enumerate(lines, start=body_line):
        if raw.startswith("## "):
            starts.append((offset, number, raw))
        offset += len(raw)

    seen_asks: set[str] = set()
    for index, (start, number, heading) in enumerate(starts):
        end = starts[index + 1][0] if index + 1 < len(starts) else len(text)
        m = HEADING_RE.match(heading.rstrip("\n"))
        if not m:
            raise ReviewParseError(path, number, f"malformed block heading {heading.strip()!r}",
                                   "write `## <type>` or `## <type> <argument>`")
        block_type, head = m.group(1), (m.group(2) or "").strip()
        if block_type not in BLOCK_TYPES:
            raise ReviewParseError(path, number, f"unknown block type {block_type!r}",
                                   "known types: " + ", ".join(sorted(BLOCK_TYPES)))

        heading_end = start + len(heading)
        block = Block(
            type=block_type, head=head, line=number,
            start=start, heading_end=heading_end, end=end, raw=text[start:end],
        )
        _parse_body(block, text[heading_end:end], number + 1, path)
        _validate_block(block, path, seen_asks)
        entry.blocks.append(block)

    return entry


def parse_file(path: Path) -> Entry:
    # `newline=""` disables universal-newline translation, so the file's own line ending reaches the parser
    # and can be put back on write.
    # Opened rather than `read_text`, whose `newline` argument only exists from 3.13.
    with path.open(encoding="utf-8", newline="") as f:
        return parse_text(f.read(), path, slug=path.stem)
