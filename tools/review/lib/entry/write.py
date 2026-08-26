"""Writing entry files without rewriting them.

The tool never re-serializes a hand-authored entry.
Every mutation below is a **splice**: edits computed against the parsed block spans and applied to the original text last-to-first,
so every byte the edit did not target survives exactly as it was written.

That is not tidiness.
A re-serializer that normalizes whitespace would change an ask's text,
which would in turn report a finalized question as modified — the tool accusing the maintainer of an edit the tool itself made.
"""

from __future__ import annotations

from pathlib import Path

from ..core.atomic import write_atomic
from .askhash import hash_ask
from .grammar import CONTEXT_TIERS, WORD_LIMITS, ReviewParseError
from .parse import Entry, parse_text


def _splice(text: str, edits: list[tuple[int, int, str]]) -> str:
    """Apply (start, end, replacement) edits to `text`, last-to-first so earlier offsets stay valid."""
    out = text
    for start, end, replacement in sorted(edits, key=lambda e: e[0], reverse=True):
        out = out[:start] + replacement + out[end:]
    return out


def restore_newlines(entry: Entry, text: str) -> str:
    """Re-apply the entry file's own line ending to spliced text.

    Offsets are computed against LF-normalized text, so without this a first stamp would convert
    a CRLF file wholesale — which is exactly the re-serialization this module exists to avoid.
    """
    return text.replace("\n", entry.newline) if entry.newline != "\n" else text


def stamp_rounds(entry: Entry, round_number: int) -> str | None:
    """Give every unstamped block a `round:`, returning the new text, or None when nothing needed one.

    A block's round is what lets the UI draw the divider between what the maintainer has already read and what is new.
    """
    edits = []
    for block in entry.blocks:
        if "round" in block.attrs:
            continue
        edits.append((block.heading_end, block.heading_end, f"round: {round_number}\n"))
    if not edits:
        return None
    return restore_newlines(entry, _splice(entry.text, edits))


def append_text(entry: Entry, addition: str) -> str:
    """Append blocks to an entry, keeping exactly one blank line before them."""
    body = entry.text.rstrip("\n")
    return restore_newlines(entry, f"{body}\n\n{addition.strip()}\n")


def immutability_violations(entry: Entry, finalized: dict[str, str]) -> list[str]:
    """Asks whose wording moved after an answer was finalized against it.

    `finalized` maps ask name to the prompt hash recorded when the answer was frozen.
    """
    violations = []
    for block in entry.asks:
        recorded = finalized.get(block.name)
        if recorded and hash_ask(block) != recorded:
            violations.append(block.name)
    return violations


def check_immutable(entry: Entry, finalized: dict[str, str]) -> None:
    """Raise if a finalized ask has been reworded, naming the remedy rather than only the rule."""
    violations = immutability_violations(entry, finalized)
    if not violations:
        return
    name = violations[0]
    block = entry.ask(name)
    raise ReviewParseError(
        entry.path, block.line if block else 1,
        f"ask {name!r} has been answered and finalized, so its wording is fixed",
        f"revert the text, or add `## ask <new-name>` with `follows: {name}` and ask the follow-up there",
    )


def check_supersedes(entry: Entry, finalized: set[str]) -> None:
    """Raise if an answered ask has been superseded, naming the remedy.

    `supersedes:` on an answered ask would walk straight around the immutability guard: the old question renders
    struck, the new one takes its place, and the answer that was given to the old wording sits under a question
    the maintainer never saw.
    An ask that has never been answered may be retired freely, since nothing was promised about it.
    """
    for block in entry.blocks:
        if not block.is_ask or not block.is_superseded or block.name not in finalized:
            continue
        raise ReviewParseError(
            entry.path, block.line,
            f"ask {block.name!r} has been answered, so it cannot be superseded",
            f"drop the `supersedes:`, and add `## ask <new-name>` with `follows: {block.name}` instead",
        )


def missing_context_tiers(entry: Entry) -> list[str]:
    """The context tiers this entry does not carry, in reading order.

    An entry is answered on its own, out of order, by someone who is not carrying the changeset in their head.
    All three tiers are what make that possible, and each is scoped to *this entry's subject* rather than to the change
    as a whole — otherwise every cold tier restates the same paragraph and nobody opens one again.
    """
    present = {block.type for block in entry.blocks}
    return [tier for tier in CONTEXT_TIERS if tier not in present]


def word_warnings(entry: Entry) -> list[str]:
    """Context tiers past the length that keeps them worth collapsing."""
    out = []
    for block in entry.blocks:
        limit = WORD_LIMITS.get(block.type)
        if limit is None:
            continue
        words = len(block.prose.split())
        if words > limit:
            out.append(f"{entry.slug}: `{block.type}` is {words} words, past the {limit} that keeps it skimmable")
    return out


def render_front(front: dict[str, str]) -> str:
    """Front matter for a generated entry, with the known keys in reading order."""
    order = ["id", "title", "group", "state", "severity", "resolved-by"]
    keys = [k for k in order if k in front] + [k for k in front if k not in order]
    lines = ["---", *(f"{k}: {front[k]}" for k in keys), "---"]
    return "\n".join(lines)


def compose(front: dict[str, str], blocks: list[str]) -> str:
    """Build a whole entry from scratch, for a generated one only.

    Never used on a file a human or an agent authored — see the module docstring for why that matters.
    """
    body = "\n\n".join(block.strip() for block in blocks if block.strip())
    return f"{render_front(front)}\n\n{body}\n"


def write_entry(path: Path, text: str) -> Entry:
    """Write an entry and hand back the parse of what actually landed, so a caller cannot drift from the file."""
    write_atomic(path, text)
    return parse_text(text, path, slug=path.stem)
