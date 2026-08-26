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
    ACK_PROMPT,
    ACK_PROMPT_LATER,
    ASK_NAME_RE,
    ack_name,
    ATTR_RE,
    BLOCK_TYPES,
    FENCE_RE,
    FRONT_KNOWN,
    FRONT_REQUIRED,
    HEADING_RE,
    SEVERITIES,
    SHOW_KINDS,
    STATES,
    Option,
    ReviewParseError,
    canonical_block_name,
    derived_name,
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
    block_name: str = ""
    effective_round: int = 0
    superseded_by: str = ""

    @property
    def name(self) -> str:
        """An ask's answer key."""
        return self.head.strip()

    @property
    def anchor(self) -> str:
        """This block's identity within its entry: the round it was written in, and its name.

        Stable because rounds freeze and blocks are only ever appended, so a later block never renumbers an earlier one.
        """
        return f"r{self.effective_round}/{self.block_name}"

    @property
    def change_ids(self) -> list[str]:
        return self.head.split()

    @property
    def discharges(self) -> list[str]:
        return self.attrs.get("discharges", "").split()

    @property
    def addresses(self) -> list[str]:
        """The comment ids this block answers."""
        return self.attrs.get("addresses", "").replace(",", " ").split()

    @property
    def round(self) -> int:
        try:
            return int(self.attrs.get("round", "0"))
        except ValueError:
            return 0

    @property
    def supersedes(self) -> str:
        """The block this one replaces, if any."""
        return self.attrs.get("supersedes", "")

    @property
    def is_superseded(self) -> bool:
        return bool(self.superseded_by)

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
    def auto_acknowledged(self) -> bool:
        """Whether this entry has declared that being read is not something to record.

        Reference material is the case: a glossary is consulted rather than read through,
        so asking someone to confirm they read it is a checkbox that means nothing.
        """
        return any(b.type == "auto-acknowledge" for b in self.blocks)

    @property
    def newest_round(self) -> int:
        """The highest round any block in this entry carries, or 1 for an entry nothing has stamped yet."""
        return max((b.round for b in self.blocks), default=0) or 1

    @property
    def acknowledgement(self) -> Block | None:
        """The synthetic ask an entry carries when its newest round added material but no question.

        Two cases reach this, and they are the same case.
        An entry that never asked anything would otherwise be indistinguishable from one that was answered.
        An entry that asked something in an earlier round, was answered, and then gained a redraft or a correction
        would otherwise show that new material under a tick earned by the old question.

        Keyed on the round, so each round's material is acknowledged on its own and an earlier acknowledgement
        cannot stand in for a later one.
        It never appears in the file, so no entry has to remember to write one.
        """
        if self.state != "open" or self.auto_acknowledged:
            return None
        latest = self.newest_round
        # An unstamped block belongs to the round about to stamp it, which `newest_round` reports as 1 —
        # so a freshly written entry's ask counts as covering it rather than being a round behind.
        if any(b.is_ask and (b.round or 1) == latest for b in self.blocks):
            return None
        first_time = not any(b.is_ask for b in self.blocks)
        return Block(type="ask", head=ack_name(latest),
                     prose=ACK_PROMPT if first_time else ACK_PROMPT_LATER,
                     options=[Option(kind="check", label="Read and acknowledged")])

    @property
    def live_blocks(self) -> list[Block]:
        """The blocks that still say what this entry says, with everything superseded left out.

        What an agent reads back, and what the artifact carries.
        The page shows both, because the maintainer needs to see what it said when they read it.
        """
        return [b for b in self.blocks if not b.is_superseded]

    @property
    def asks(self) -> list[Block]:
        """Every question this entry poses, the synthetic acknowledgement included.

        Downstream code counts, answers and reports asks without caring which kind it has,
        which is the point: an acknowledgement is progress in exactly the way an answer is.

        A superseded ask is not among them: it has been retired, and only one that was never answered may be.
        """
        real = [b for b in self.blocks if b.is_ask and not b.is_superseded]
        ack = self.acknowledgement
        return real + ([ack] if ack is not None else [])

    def ask(self, name: str) -> Block | None:
        return next((b for b in self.asks if b.name == name), None)

    def block(self, ref: str) -> Block | None:
        """A block by its identity, as `r2/prose#1` or as a bare `prose#1`.

        A bare name is resolved against every round, newest first, since it can only be ambiguous across rounds —
        within one, names are unique.
        """
        round_part, _, name = ref.rpartition("/")
        wanted = canonical_block_name(name)
        want_round = int(round_part[1:]) if round_part[1:].isdigit() and round_part.startswith("r") else 0
        matches = [b for b in self.blocks if canonical_block_name(b.block_name) == wanted]
        if want_round:
            matches = [b for b in matches if b.effective_round == want_round]
        return max(matches, key=lambda b: b.effective_round, default=None)

    def referenced_changes(self) -> list[str]:
        """Every change id this entry names, from `changes` heads and `discharges` attributes alike."""
        out: list[str] = []
        for block in self.blocks:
            if block.type == "changes":
                out.extend(block.change_ids)
            out.extend(block.discharges)
        return out

    def addressed_comments(self) -> set[str]:
        """Every comment id a block in this entry claims to answer."""
        out: set[str] = set()
        for block in self.blocks:
            out.update(block.addresses)
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


def _fenced_lines(lines: list[str], first_line: int, path: Path) -> set[int]:
    """The line numbers sitting inside a fenced code block, where a `## ` is prose rather than a block start."""
    inside: set[int] = set()
    fence = ""
    opened_at = 0
    for number, raw in enumerate(lines, start=first_line):
        stripped = raw.lstrip(" ")
        indented = len(raw) - len(stripped)
        m = FENCE_RE.match(stripped) if indented <= 3 else None
        if not fence:
            if m:
                fence, opened_at = m.group(1), number
                inside.add(number)
            continue
        inside.add(number)
        # A closing fence is at least as long as its opener, of the same character, and carries nothing else.
        if m and m.group(1)[0] == fence[0] and len(m.group(1)) >= len(fence) and not stripped[len(m.group(1)):].strip():
            fence = ""
    if fence:
        raise ReviewParseError(path, opened_at, "a fenced code block is never closed",
                               f"add a closing `{fence}` line, or the rest of the entry is read as code")
    return inside


def _assign_block_names(entry: Entry, path: Path) -> None:
    """Give every block its derived name, and refuse two blocks of one round that answer to the same one.

    A block that carries `name:` keeps it; an ask is named by its heading, which the grammar already keeps unique.
    Everything else is named after its type, indexed only where that type repeats within the round.
    """
    latest = entry.newest_round
    groups: dict[tuple[int, str], list[Block]] = {}
    for block in entry.blocks:
        # An unstamped block belongs to the round about to stamp it, which is the same rule `acknowledgement` uses.
        block.effective_round = block.round or latest
        groups.setdefault((block.effective_round, block.type), []).append(block)

    for (_, block_type), blocks in groups.items():
        indexed = len(blocks) > 1
        for ordinal, block in enumerate(blocks, start=1):
            explicit = block.attrs.get("name", "")
            if explicit and not ASK_NAME_RE.match(canonical_block_name(explicit)):
                raise ReviewParseError(path, block.line, f"{explicit!r} is not a usable block name",
                                       "lowercase letters, digits and dashes")
            if explicit:
                block.block_name = explicit
            elif block.is_ask:
                block.block_name = block.name
            else:
                block.block_name = derived_name(block_type, ordinal, indexed=indexed)

    seen: dict[tuple[int, str], Block] = {}
    for block in entry.blocks:
        key = (block.effective_round, canonical_block_name(block.block_name))
        clash = seen.get(key)
        if clash is not None:
            raise ReviewParseError(
                path, block.line, f"two blocks of round {block.effective_round} are both named {key[1]!r}",
                f"the other is on line {clash.line}; a block name is the anchor a comment or a `supersedes:` uses, "
                f"so it must be unique within a round",
            )
        seen[key] = block


def _resolve_supersedes(entry: Entry, path: Path) -> None:
    """Point every `supersedes:` at the block it replaces, and refuse one that names nothing.

    Only an earlier block in the same entry can be a target.
    Within one entry is what makes the rendering obvious — the struck original and its replacement are on the same
    screen — and a correction that lands somewhere the reader is not is the problem this feature exists to solve.
    """
    for index, block in enumerate(entry.blocks):
        ref = block.supersedes
        if not ref:
            continue
        _, _, name = ref.rpartition("/")
        wanted = canonical_block_name(name)
        round_part, _, _ = ref.rpartition("/")
        want_round = int(round_part[1:]) if round_part.startswith("r") and round_part[1:].isdigit() else 0

        candidates = [
            b for b in entry.blocks[:index]
            if canonical_block_name(b.block_name) == wanted and (not want_round or b.effective_round == want_round)
        ]
        if not candidates:
            raise ReviewParseError(
                path, block.line, f"`supersedes: {ref}` names no earlier block in this entry",
                "a correction replaces a block above it, in the same entry; `review show` lists the block names",
            )
        target = candidates[-1]
        if target.is_superseded:
            raise ReviewParseError(
                path, block.line, f"{ref!r} has already been superseded",
                f"supersede {block.block_name!r}'s current replacement instead, or name it explicitly",
            )
        target.superseded_by = block.anchor


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
    fenced = _fenced_lines(lines, body_line, path)
    starts: list[tuple[int, int, str]] = []
    offset = body_offset
    for number, raw in enumerate(lines, start=body_line):
        if raw.startswith("## ") and number not in fenced:
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
            # A block type is lowercase kebab-case, so anything else here is usually a markdown heading
            # inside a block whose body is markdown — an `artifact` above all.
            # `## ` opens a block wherever it lands.
            prose_heading = block_type[:1].isupper() or "_" in block_type
            hint = ("known types: " + ", ".join(sorted(BLOCK_TYPES)))
            remedy = (f"if that is a heading inside a block's body, write it as `### {block_type}`; "
                      "if it is a sample of the block grammar, indent it four spaces. "
                      "`## ` starts a block outside a fenced block, whatever it lands in. ")
            if prose_heading:
                hint = remedy + hint
            else:
                hint = "if this is not meant as a block, " + remedy + hint
            raise ReviewParseError(path, number, f"unknown block type {block_type!r}", hint)

        heading_end = start + len(heading)
        block = Block(
            type=block_type, head=head, line=number,
            start=start, heading_end=heading_end, end=end, raw=text[start:end],
        )
        _parse_body(block, text[heading_end:end], number + 1, path)
        _validate_block(block, path, seen_asks)
        entry.blocks.append(block)

    _assign_block_names(entry, path)
    _resolve_supersedes(entry, path)
    return entry


def parse_file(path: Path) -> Entry:
    # `newline=""` disables universal-newline translation, so the file's own line ending reaches the parser
    # and can be put back on write.
    # Opened rather than `read_text`, whose `newline` argument only exists from 3.13.
    with path.open(encoding="utf-8", newline="") as f:
        return parse_text(f.read(), path, slug=path.stem)
