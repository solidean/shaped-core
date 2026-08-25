"""What a review ends by producing.

Each goal ends somewhere different, and the difference is not cosmetic.
A comment for someone else must stand alone for a reader who was never in the conversation.
A session that lands its own changes wants a work order, keyed to the changes so `sync` can verify each one.
A design review wants the decisions, because the code that will carry them does not exist yet.

Every artifact here is a **draft**.
The tool can assemble what was decided; it cannot know which three of the twelve points are the ones worth someone's afternoon.
"""

from __future__ import annotations

from ..core.config import ReviewConfig
from ..entry.answers import AnswerFile
from ..entry.parse import Entry

_SKIP_GROUPS = {"meta", "lgtm", "finalize", "framing"}


def _decisions(entry: Entry, answers: AnswerFile) -> list[tuple[str, list[str], str]]:
    """(ask name, chosen options, free text) for every question this entry has a real answer to."""
    out = []
    for block in entry.asks:
        answer = answers.get(block.name)
        if answer is None or answer.is_empty:
            continue
        out.append((block.name, list(answer.selected), answer.text.strip()))
    return out


def _answered(pairs: list[tuple[Entry, AnswerFile]]) -> list[tuple[Entry, AnswerFile, list]]:
    rows = []
    for entry, answers in pairs:
        if entry.state != "open":
            continue
        decisions = _decisions(entry, answers)
        if decisions:
            rows.append((entry, answers, decisions))
    return rows


def pr_comment(cfg: ReviewConfig, pairs: list[tuple[Entry, AnswerFile]]) -> str:
    """A single standalone comment: instructions only, no conversation, no open questions.

    Context tiers and the overview are dropped on purpose — they exist so the maintainer could judge a point,
    and they tell the author nothing they do not already know.
    """
    lines = ["<!-- draft: read it before posting; the tool assembled it, it did not decide it -->", ""]
    number = 0
    for entry, _, decisions in _answered(pairs):
        if entry.group in _SKIP_GROUPS:
            continue
        number += 1
        severity = f" ({entry.severity})" if entry.severity else ""
        lines.append(f"**{number}. {entry.title}**{severity}")
        for _, chosen, text in decisions:
            for option in chosen:
                lines.append(f"- {option}")
            if text:
                lines.append(f"- {text}")
        lines.append("")

    if number == 0:
        lines.append("_No decided points yet._")
    return "\n".join(lines).rstrip() + "\n"


def work_order(cfg: ReviewConfig, pairs: list[tuple[Entry, AnswerFile]]) -> str:
    """A todo list for this session, keyed to the changes each point discharges.

    The change ids are what makes it checkable afterwards: once the fix lands, `sync` marks them superseded.
    """
    lines = [f"# Work order — {cfg.name}", ""]
    number = 0
    for entry, _, decisions in _answered(pairs):
        if entry.group in _SKIP_GROUPS:
            continue
        number += 1
        lines.append(f"## {number}. {entry.title}  ({entry.group}"
                     + (f"/{entry.severity}" if entry.severity else "") + ")")
        for name, chosen, text in decisions:
            block = entry.ask(name)
            discharges = f"  [{' '.join(block.discharges)}]" if block and block.discharges else ""
            for option in chosen:
                lines.append(f"- [ ] {option}{discharges}")
            if text and chosen:
                lines.append(f"      note: {text}")
            elif text:
                lines.append(f"- [ ] {text}{discharges}")
        if entry.front.get("resolved-by"):
            lines.append(f"      already landed in {entry.front['resolved-by']}")
        lines.append("")

    if number == 0:
        lines.append("_Nothing decided yet._")
    return "\n".join(lines).rstrip() + "\n"


def design_summary(cfg: ReviewConfig, pairs: list[tuple[Entry, AnswerFile]]) -> str:
    """What was agreed, and what is still open — the input to a plan rather than to an edit."""
    lines = [f"# {cfg.title or cfg.name} — decisions", ""]
    settled: list[str] = []
    open_points: list[str] = []

    for entry, answers in pairs:
        if entry.state != "open":
            continue
        decisions = _decisions(entry, answers)
        answered_names = {name for name, _, _ in decisions}
        for name, chosen, text in decisions:
            head = f"**{entry.title} / {name}**"
            body = "; ".join([*chosen, *([text] if text else [])])
            settled.append(f"- {head}: {body}")
        for block in entry.asks:
            if block.name not in answered_names:
                open_points.append(f"- **{entry.title} / {block.name}** — not decided")

    lines += ["## Settled", ""] + (settled or ["_nothing yet_"])
    lines += ["", "## Still open", ""] + (open_points or ["_nothing_"])
    return "\n".join(lines).rstrip() + "\n"


ARTIFACTS = {
    "pr-comment": pr_comment,
    "land-changes": work_order,
    "design": design_summary,
}
