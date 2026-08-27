"""`show` — an entry, its answers, and the round it is in, as one plain-text read.

This is what makes a review survivable across sessions.
An agent that has lost its context reads the entries back and sees the whole conversation, not only what it wrote.
"""

from __future__ import annotations

import argparse

import tools.review as review

from . import args as a
from .context import Context

NAME = "show"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Print an entry with its answers folded in")
    a.review_name(p)
    p.add_argument("entry", nargs="?", default="",
                   help="an entry id, slug or substring; omit to list every entry")
    p.add_argument("--all", action="store_true", help="print every entry in full")
    p.add_argument("--unanswered", action="store_true", help="only entries with an ask nobody has answered")
    p.add_argument("--history", action="store_true",
                   help="include superseded blocks, which is what the maintainer read rather than what the entry says")
    return p


def _select(entries: list[review.Entry], token: str) -> list[review.Entry]:
    exact = [e for e in entries if e.id == token or e.slug == token]
    if exact:
        return exact
    lowered = token.lower()
    return [e for e in entries if lowered in e.slug.lower() or lowered in e.title.lower()]


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    entries = ctx.entries(paths)
    if not entries:
        print(f"no entries yet under {ctx.rel(paths.entries_dir)}")
        return

    if args.unanswered:
        def pending(entry: review.Entry) -> bool:
            answers = ctx.answers(paths, entry)
            return any((a := answers.get(b.name)) is None or a.is_empty for b in entry.asks)
        entries = [e for e in entries if e.state == "open" and pending(e)]

    if args.entry:
        entries = _select(entries, args.entry)
        if not entries:
            ctx.die(f"no entry matches {args.entry!r}")

    full = args.all or bool(args.entry) or args.unanswered
    pairs = [(entry, ctx.answers(paths, entry)) for entry in entries]

    if not full:
        print(review.render_summary(pairs))
        groups = review.groups_for(cfg.goals)
        unplaced = sorted({e.group for e in entries} - set(groups))
        if unplaced:
            print(review.console.yellow(f"groups outside this review's skeleton: {', '.join(unplaced)}"))
        return

    for entry, answers in pairs:
        print(review.render_entry(entry, answers, history=args.history))
        for warning in review.word_warnings(entry):
            print(review.console.yellow(warning))
