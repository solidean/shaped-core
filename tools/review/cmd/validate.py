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

    if cfg.has_changeset:
        problems.extend(ctx.check_references(paths, entries))

    for entry in entries:
        warnings.extend(review.word_warnings(entry))
        answers = ctx.answers(paths, entry)
        for name in sorted(answers.answers):
            if entry.ask(name) is None:
                warnings.append(f"{entry.slug}: an answer to {name!r} has no ask; `delta` will orphan it")

    groups = set(review.groups_for(cfg.goals))
    unplaced = sorted({e.group for e in entries} - groups)
    if unplaced:
        warnings.append(f"groups outside this review's skeleton: {', '.join(unplaced)}")

    for warning in warnings:
        print(review.console.yellow(f"warning: {warning}"))
    for problem in problems:
        print(review.console.red(f"error: {problem}"))

    if problems:
        print(review.console.red(f"\n{len(problems)} unresolved reference(s) across {len(entries)} entries"))
        raise SystemExit(1)
    if not args.quiet:
        print(review.console.green(f"{len(entries)} entries parse, every reference resolves"))
