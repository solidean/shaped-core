"""An entry and its answers, joined into the one document the maintainer reads.

The two live in separate files so the agent and the server never write the same one.
Joining them here is what makes that invisible: every answer renders inline, directly under the question it answers,
and a later round appends below rather than rewriting anything above.

A question already finalized renders as a card rather than a form.
Its wording is fixed at that point, and so is what was said about it — a follow-up is a new ask further down.
"""

from __future__ import annotations

import html
from pathlib import Path

from ..core.paths import ReviewPaths
from ..entry.answers import Answer, AnswerFile
from ..entry.parse import Block, Entry
from .highlight import highlight_diff
from .markdown import render as render_markdown

_COLLAPSED_TIERS = ("context/cold", "context/repo")

_TIER_LABEL = {
    "context/cold": "New to this change and the codebase",
    "context/repo": "Knows the codebase, new to this change",
    "context/delta": "Since the previous entries",
}


def _esc(text: str) -> str:
    return html.escape(text, quote=True)


def _change_card(change, body: str, *, open_by_default: bool) -> str:
    summary = _esc(change.summary or change.path)
    reason = f'<div class="change-reason">{_esc(change.reason)}</div>' if change.reason else ""
    if not body:
        return (f'<div class="change"><div class="change-head"><code>{_esc(change.id)}</code>'
                f'<span class="change-sum">{summary}</span></div>{reason}</div>')
    return (
        f'<details class="change"{" open" if open_by_default else ""}><summary class="change-head"><code>{_esc(change.id)}</code>'
        f'<span class="change-sum">{summary}</span></summary>{reason}'
        f'{highlight_diff(body, path=change.path)}</details>'
    )


def _answer_card(answer: Answer) -> str:
    state = "tentative" if answer.tentative else f"round {answer.round}"
    chosen = "".join(f"<li>{_esc(choice)}</li>" for choice in answer.selected)
    chosen_html = f"<ul class=\"answer-choices\">{chosen}</ul>" if chosen else ""
    text_html = f'<div class="answer-text">{_esc(answer.text)}</div>' if answer.text.strip() else ""
    return (
        f'<div class="answer-card"><div class="answer-head">Your answer '
        f'<span class="answer-state">{_esc(state)}</span></div>{chosen_html}{text_html}</div>'
    )


def _ask_form(entry: Entry, block: Block, answer: Answer | None, prompt_hash: str) -> str:
    selected = set(answer.selected) if answer else []
    text = answer.text if answer else ""

    inputs = []
    for index, option in enumerate(block.options):
        control = "radio" if option.kind == "radio" else "checkbox"
        group = f"{entry.slug}::{block.name}" if option.kind == "radio" else f"{entry.slug}::{block.name}::{index}"
        checked = " checked" if option.label in selected else ""
        rec = '<span class="rec">recommended</span>' if option.recommended else ""
        inputs.append(
            f'<label class="opt opt-{option.kind}">'
            f'<input type="{control}" name="{_esc(group)}" value="{_esc(option.label)}"{checked}>'
            f'<span class="opt-label">{_esc(option.label)}</span>{rec}</label>'
        )

    return (
        f'<form class="ask-form" data-entry="{_esc(entry.slug)}" data-ask="{_esc(block.name)}" '
        f'data-hash="{_esc(prompt_hash)}">'
        f'<div class="opts">{"".join(inputs)}</div>'
        f'<textarea class="freeform" rows="3" placeholder="anything else — always here, always optional"'
        f'>{_esc(text)}</textarea>'
        f'<div class="ask-status" aria-live="polite"></div>'
        f"</form>"
    )


def _block_html(entry: Entry, block: Block, ctx: dict) -> str:
    repo: Path = ctx["repo"]

    if block.type in _TIER_LABEL:
        body = render_markdown(block.prose, repo=repo)
        label = _TIER_LABEL[block.type]
        if block.type in _COLLAPSED_TIERS:
            return (f'<details class="tier tier-{block.type.split("/")[1]}">'
                    f'<summary>{_esc(label)}</summary><div class="tier-body">{body}</div></details>')
        # Drawn as a rule rather than a label: this is where a round's new material starts, and it has to be findable by eye.
        return (f'<div class="tier-delta-rule"><span>{_esc(label)}</span></div>'
                f'<div class="tier tier-delta">{body}</div>')

    if block.type == "changes":
        visible = block.attrs.get("show", "collapsed") == "visible"
        cards = []
        for change_id in block.change_ids:
            change = ctx["ledger"].resolve(change_id)
            if change is None:
                cards.append(f'<div class="change missing"><code>{_esc(change_id)}</code> is not in the ledger</div>')
                continue
            body = ""
            diff_path: Path = ctx["paths"].change_diff(change.id)
            if change.has_body and diff_path.is_file():
                body = diff_path.read_text(encoding="utf-8", errors="replace")
            cards.append(_change_card(change, body, open_by_default=visible))
        commentary = render_markdown(block.prose, repo=repo)
        return f'<section class="changes">{commentary}{"".join(cards)}</section>'

    if block.type == "code":
        return render_markdown(block.prose, repo=repo)

    if block.type == "recommendation":
        return (f'<aside class="recommendation"><div class="rec-label">Recommendation</div>'
                f'{render_markdown(block.prose, repo=repo)}</aside>')

    if block.type == "ask":
        answers: AnswerFile = ctx["answers"]
        answer = answers.get(block.name)
        prompt_hash = ctx["hash_of"](block)
        prose = render_markdown(block.prose, repo=repo)
        follows = block.attrs.get("follows", "")
        follows_html = (f'<div class="follows">follows <code>{_esc(follows)}</code></div>' if follows else "")
        discharges = ""
        if block.discharges:
            ids = " ".join(f"<code>{_esc(i)}</code>" for i in block.discharges)
            discharges = f'<div class="discharges">discharges {ids}</div>'

        if answer is not None and not answer.tentative:
            body = _answer_card(answer)
        else:
            body = _ask_form(entry, block, answer, prompt_hash)

        return (f'<section class="ask" id="ask-{_esc(block.name)}">{follows_html}'
                f'<div class="ask-prose">{prose}</div>{discharges}{body}</section>')

    return f'<div class="prose">{render_markdown(block.prose, repo=repo)}</div>'


def render_entry(entry: Entry, answers: AnswerFile, *, repo: Path, paths: ReviewPaths, ledger, hash_of) -> str:
    """The whole entry as HTML: blocks in order, answers inline, a divider where a round begins."""
    ctx = {"repo": repo, "paths": paths, "ledger": ledger, "answers": answers, "hash_of": hash_of}

    # A severity that repeats the group says nothing twice — `design/design`, `docs/docs` — so only a differing one is drawn.
    show_severity = entry.severity and entry.severity != entry.group
    severity = f'<span class="sev sev-{_esc(entry.severity)}">{_esc(entry.severity)}</span>' if show_severity else ""
    state = f'<span class="state state-{_esc(entry.state)}">{_esc(entry.state)}</span>' if entry.state != "open" else ""
    head = (f'<header class="entry-head"><div class="entry-id">{_esc(entry.id)}</div>'
            f'<h1>{_esc(entry.title)}</h1><div class="entry-meta">'
            f'<span class="group">{_esc(entry.group)}</span>{severity}{state}</div></header>')

    parts = [head]
    last_round = 0
    for block in entry.blocks:
        if block.round and block.round != last_round:
            if last_round:
                parts.append(f'<div class="round-divider"><span>round {block.round}</span></div>')
            last_round = block.round
        parts.append(_block_html(entry, block, ctx))

    if answers.orphans:
        rows = "".join(
            f'<li><code>{_esc(key)}</code>{_answer_card(answer)}</li>'
            for key, answer in sorted(answers.orphans.items())
        )
        parts.append(f'<section class="orphans"><h2>Answers whose question changed</h2><ul>{rows}</ul></section>')

    return f'<article class="entry" data-slug="{_esc(entry.slug)}">{"".join(parts)}</article>'
