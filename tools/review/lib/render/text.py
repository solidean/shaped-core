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
    return f"## {block.type}{head}{stamp}"


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


def render_entry(entry: Entry, answers: AnswerFile) -> str:
    """One entry and its answers, in the order they were written."""
    header = [
        _RULE,
        f"{entry.id}  {entry.title}",
        f"group: {entry.group}   state: {entry.state}" + (f"   severity: {entry.severity}" if entry.severity else ""),
    ]
    if entry.front.get("resolved-by"):
        header.append(f"resolved-by: {entry.front['resolved-by']}")
    header.append(_RULE)

    out = list(header)
    for block in entry.blocks:
        out.append("")
        if block.is_ask:
            out.extend(_render_ask(block, answers))
            continue
        out.append(_block_heading(block))
        if block.prose:
            out.append("")
            out.append(block.prose)

    return "\n".join(out) + "\n"


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
