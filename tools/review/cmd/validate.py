"""`validate` — check every entry before handing the review over.

The other commands surface a problem when they happen to trip over it.
This one goes looking, which is what you want before serving a round: a mistyped change id is a discharge
that silently does not discharge, and the coverage report would report progress that was never made.
"""

from __future__ import annotations

import argparse

import tools.review as review

from . import args as a
from .context import Context

NAME = "validate"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Check every entry parses and every reference resolves")
    a.review_name(p)
    p.add_argument("--quiet", action="store_true", help="report nothing when everything is fine")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    entries = ctx.entries(paths)

    problems: list[str] = []
    warnings: list[str] = []
    thin = False

    if cfg.has_changeset:
        problems.extend(ctx.check_references(paths, entries))

    for entry in entries:
        warnings.extend(review.word_warnings(entry))
        if review.requires_context(entry.group):
            absent = review.missing_context_tiers(entry)
            if absent:
                thin = True
                problems.append(f"{entry.slug}: no {', '.join(absent)}")
        answers = ctx.answers(paths, entry)
        for name in sorted(answers.answers):
            if entry.ask(name) is None:
                warnings.append(f"{entry.slug}: an answer to {name!r} has no ask; `delta` will orphan it")

    groups = set(review.groups_for(cfg.goals))
    unplaced = sorted({e.group for e in entries} - groups)
    if unplaced:
        warnings.append(
            f"groups outside this review's skeleton: {', '.join(unplaced)}. "
            f"This review's groups are: {', '.join(review.groups_for(cfg.goals))}"
        )

    for warning in warnings:
        print(review.console.yellow(f"warning: {warning}"))
    for problem in problems:
        print(review.console.red(f"error: {problem}"))

    if thin:
        # Said once rather than per entry: a rule repeated twelve times reads as twelve rules.
        print(review.console.dim(
            f"\nEvery entry outside {', '.join(sorted(review.CONTEXT_EXEMPT_GROUPS))} carries all three context tiers,"
            f" so it can be answered on its own, out of order."
        ))
        print(review.console.dim(
            "Scope each tier to that entry's own subject rather than to the change as a whole,"
            " or every cold tier restates the same paragraph and nobody opens one again."
        ))

    if problems:
        print(review.console.red(f"\n{len(problems)} problem(s) across {len(entries)} entries"))
        raise SystemExit(1)
    if not args.quiet:
        print(review.console.green(f"{len(entries)} entries parse, every reference resolves"))
