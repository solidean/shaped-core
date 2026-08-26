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
from ..entry.answers import Answer, AnswerFile, Comment
from ..entry.parse import Block, Entry
from .highlight import highlight_code, highlight_diff
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


def _comment_card(comment: Comment) -> str:
    """One remark, under whatever it was left on."""
    state = "not sent yet" if comment.tentative else f"sent in round {comment.round}"
    where = f'<span class="comment-where">{_esc(comment.where())}</span>' if comment.is_line else ""
    return (
        f'<div class="comment-card" data-comment="{_esc(comment.id)}">'
        f'<div class="comment-head"><code>{_esc(comment.id)}</code>{where}'
        f'<span class="comment-state">{_esc(state)}</span></div>'
        f'<div class="comment-text">{_esc(comment.text)}</div></div>'
    )


def _comment_slot(anchor: str, comments: list[Comment]) -> str:
    """The affordance for leaving a remark here, plus whatever has already been left.

    On every block rather than only on an ask: the context tiers are where "why did we do it this way" lands,
    and until now that question had nowhere to go but the text box of an unrelated question.
    """
    cards = "".join(_comment_card(c) for c in comments)
    return (
        f'<div class="comment-slot" data-anchor="{_esc(anchor)}">'
        f'<button class="comment-add" type="button" title="comment on this block">comment</button>'
        f'<div class="comment-list">{cards}</div></div>'
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


def _source_slice(repo: Path, spec: str) -> tuple[str, str]:
    """(path, the lines it names), for the `source:` an example shows before its output."""
    path, _, span = spec.partition(":")
    target = repo / path.strip()
    if not target.is_file():
        return path.strip(), ""
    lines = target.read_text(encoding="utf-8", errors="replace").splitlines()
    first, _, last = span.partition("-")
    start = max(int(first) - 1, 0) if first.strip().isdigit() else 0
    end = int(last) if last.strip().isdigit() else (start + 40 if first.strip().isdigit() else len(lines))
    return path.strip(), "\n".join(lines[start:end])


def _example_html(block: Block, ctx: dict) -> str:
    """One example, its source, and what running it printed.

    The provenance line is the point of the block.
    Output without the command, the commit and the time is an unverifiable claim, in a tool whose whole premise
    is that claims are checkable — and the difference between "I ran it" and "I read it and it looked right"
    is one nobody can make from the outside.
    """
    repo: Path = ctx["repo"]
    paths: ReviewPaths = ctx["paths"]
    ran, shown = block.attrs.get("run", ""), block.attrs.get("cmd", "")
    command = ran or shown

    head = (f'<div class="example-head"><span class="example-name">{_esc(block.head or "example")}</span>'
            f'<code class="example-cmd">{_esc(command)}</code></div>')

    source = ""
    if block.attrs.get("source"):
        path, body = _source_slice(repo, block.attrs["source"])
        if body:
            source = (f'<div class="code-label">{_esc(block.attrs["source"])}</div>'
                      f'<pre class="pg"><code>{highlight_code(body, path=path)}</code></pre>')

    output = ""
    name = block.attrs.get("output", "")
    if name:
        target = paths.root / name
        if target.is_file():
            if target.suffix.lower() in (".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg"):
                output = f'<img class="example-shot" src="/attachments/{_esc(target.name)}" alt="{_esc(command)}">'
            else:
                body = target.read_text(encoding="utf-8", errors="replace")
                output = f'<pre class="example-out">{_esc(body)}</pre>'
        else:
            output = f'<div class="example-missing">{_esc(name)} is not in this review folder</div>'

    state = block.attrs.get("status", "")
    if not ran:
        # A fact about which key was used, not an honour system: the tool knows it did not produce this.
        note = "not reproduced by the tool" if output else "not run here — the command is for you to run"
    elif state == "failed":
        note = f"exited non-zero, at {block.attrs.get('sha', '?')}"
    else:
        note = f"run at {block.attrs.get('sha', '?')}, {block.attrs.get('at', '')}"
    if state == "not-automatable":
        note = "cannot be captured automatically yet"

    stale = ctx.get("head", "") and block.attrs.get("sha") and not ctx["head"].startswith(block.attrs["sha"])
    provenance = (f'<div class="example-note{" stale" if stale else ""}">{_esc(note)}'
                  f'{" · the code has moved since" if stale else ""}</div>')

    commentary = render_markdown(block.prose, repo=repo)
    return f'<section class="example">{head}{commentary}{source}{output}{provenance}</section>'


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
            on_lines = [c for c in ctx["comments"] if c.change == change.id]
            if on_lines:
                cards.append(f'<div class="line-comments">{"".join(_comment_card(c) for c in on_lines)}</div>')
        commentary = render_markdown(block.prose, repo=repo)
        return f'<section class="changes">{commentary}{"".join(cards)}</section>'

    if block.type == "example":
        return _example_html(block, ctx)

    if block.type == "code":
        return render_markdown(block.prose, repo=repo)

    if block.type == "auto-acknowledge":
        note = render_markdown(block.prose, repo=repo) if block.prose.strip() else ""
        return f'<div class="auto-ack">Reference — nothing to acknowledge{note}</div>'

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


def render_entry(entry: Entry, answers: AnswerFile, *, repo: Path, paths: ReviewPaths, ledger, hash_of,
                 head: str = "") -> str:
    """The whole entry as HTML: blocks in order, answers inline, a divider where a round begins."""
    comments = sorted(answers.comments.values(), key=lambda c: (c.round, c.id))
    ctx = {"repo": repo, "paths": paths, "ledger": ledger, "answers": answers, "hash_of": hash_of,
           "comments": comments, "head": head}

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
        on_block = [c for c in comments if c.block == block.anchor]
        body = _block_html(entry, block, ctx)
        if block.is_superseded:
            # Both, rather than only the replacement: the maintainer needs to see what the entry says now
            # and what it said when they read it.
            body = (f'<details class="superseded"><summary>superseded by '
                    f'<code>{_esc(block.superseded_by)}</code></summary>{body}</details>')
        replaces = ""
        if block.supersedes:
            replaces = f'<div class="replaces">replaces <code>{_esc(block.supersedes)}</code></div>'
        parts.append(
            f'<section class="block" data-anchor="{_esc(block.anchor)}">'
            f'{replaces}{body}{_comment_slot(block.anchor, on_block)}</section>'
        )

    ack = entry.acknowledgement
    if ack is not None:
        parts.append(_block_html(entry, ack, ctx))

    stranded = [c for c in comments if not c.is_line and c.block and entry.block(c.block) is None]
    if stranded:
        parts.append(
            '<section class="orphans"><h2>Comments whose block is gone</h2>'
            + "".join(_comment_card(c) for c in stranded) + "</section>"
        )

    if answers.orphans:
        rows = "".join(
            f'<li><code>{_esc(key)}</code>{_answer_card(answer)}</li>'
            for key, answer in sorted(answers.orphans.items())
        )
        parts.append(f'<section class="orphans"><h2>Answers whose question changed</h2><ul>{rows}</ul></section>')

    return f'<article class="entry" data-slug="{_esc(entry.slug)}">{"".join(parts)}</article>'
