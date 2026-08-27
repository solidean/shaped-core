"""An entry rendered as plain text, with its answers folded in.

This is the agent's view, and it is the reason a review survives being picked up in a later session:
`review show` prints the whole conversation for an entry — the context, the question, and what the maintainer answered — in one read.
The maintainer's UI joins the same two files the same way; only the markup differs.
"""

from __future__ import annotations

from ..entry.answers import AnswerFile
from ..entry.parse import Block, Entry

_RULE = "-" * 78


def _block_heading(block: Block) -> str:
    head = f" {block.head}" if block.head else ""
    stamp = f"  [round {block.round}]" if block.round else ""
    # The name is printed because it is what a `supersedes:` and a comment anchor on, and an agent writing either
    # needs to read it off somewhere.
    name = f"  <{block.block_name}>" if block.block_name and not block.is_ask else ""
    return f"## {block.type}{head}{name}{stamp}"


def _render_ask(block: Block, answers: AnswerFile) -> list[str]:
    out = [_block_heading(block)]
    if block.discharges:
        out.append(f"discharges: {' '.join(block.discharges)}")
    if block.attrs.get("follows"):
        out.append(f"follows: {block.attrs['follows']}")
    if block.prose:
        out.append("")
        out.append(block.prose)

    if block.options:
        out.append("")
        for option in block.options:
            mark = "  (recommended)" if option.recommended else ""
            out.append(f"  [{option.kind}] {option.label}{mark}")

    answer = answers.get(block.name)
    out.append("")
    if answer is None or answer.is_empty:
        out.append("  ANSWER: (none yet)")
    else:
        state = "tentative" if answer.tentative else f"final, round {answer.round}"
        out.append(f"  ANSWER ({state}):")
        for choice in answer.selected:
            out.append(f"    * {choice}")
        if answer.text.strip():
            for line in answer.text.strip().splitlines():
                out.append(f"    | {line}")
    return out


def render_entry(entry: Entry, answers: AnswerFile, *, history: bool = False) -> str:
    """One entry and its answers, in the order they were written.

    Live blocks only by default: an agent reading back wants what the entry says now.
    `history` adds the superseded ones, which is what the maintainer read rather than what is true.
    """
    header = [
        _RULE,
        f"{entry.id}  {entry.title}",
        f"group: {entry.group}   state: {entry.state}" + (f"   severity: {entry.severity}" if entry.severity else ""),
    ]
    if entry.front.get("resolved-by"):
        header.append(f"resolved-by: {entry.front['resolved-by']}")
    header.append(_RULE)

    out = list(header)
    for block in (entry.blocks if history else entry.live_blocks):
        out.append("")
        if block.is_superseded:
            out.append(f"  (superseded by {block.superseded_by})")
        if block.is_ask:
            out.extend(_render_ask(block, answers))
        else:
            out.append(_block_heading(block))
            if block.prose:
                out.append("")
                out.append(block.prose)
        out.extend(_render_comments([c for c in answers.comments.values() if c.block == block.anchor]))

    loose = [c for c in answers.comments.values() if c.is_line or (c.block and entry.block(c.block) is None)]
    if loose:
        out.append("")
        out.append("## comments not on a block")
        out.extend(_render_comments(loose))

    return "\n".join(out) + "\n"


def _render_comments(comments: list) -> list[str]:
    """The maintainer's remarks, under whatever they were left on.

    Printed inline rather than gathered at the end: a comment is about the block above it, and an agent reading
    an entry back needs it where the thing it is about is.
    """
    out: list[str] = []
    for comment in sorted(comments, key=lambda c: c.id):
        state = "not sent yet" if comment.tentative else f"round {comment.round}"
        out.append("")
        out.append(f"  COMMENT {comment.id} ({state}, on {comment.where()}):")
        for line in comment.text.strip().splitlines():
            out.append(f"    | {line}")
    return out


def render_summary(entries: list[tuple[Entry, AnswerFile]]) -> str:
    """One line per entry: where it sits, what it is, and whether it has been answered."""
    out = []
    for entry, answers in entries:
        asks = entry.asks
        answered = sum(1 for block in asks if (a := answers.get(block.name)) and not a.is_empty)
        if not asks:
            progress = "no asks"
        else:
            progress = f"{answered}/{len(asks)} answered"
        flag = " " if entry.state == "open" else "~"
        severity = f"[{entry.severity}]" if entry.severity else ""
        out.append(f"{flag} {entry.id:<5} {entry.group:<14} {progress:<14} {severity:<10} {entry.title}")
    return "\n".join(out) + ("\n" if out else "")
