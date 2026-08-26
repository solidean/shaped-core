"""`artifact` — the exact text the review is going to publish, taken out of the entry that holds it.

A `pr-comment` review ends by posting something a person wrote, not something the tool assembled,
so the text lives in an `## artifact` block on the draft entry and this is what reads it back out.

Two callers need it and neither should be parsing entry files by hand: `post`, which publishes it,
and the adversarial check the skill prescribes, which hands the branch and this text to a subagent
with none of the review's context and asks whether it could be implemented as written.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import tools.review as review

from . import args as a
from .context import Context

NAME = "artifact"

ARTIFACT_BLOCK = "artifact"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Print the text this review will publish")
    a.review_name(p)
    p.add_argument("--write", default="", metavar="PATH", help="write it to a file as well as printing it")
    p.add_argument("--quiet", action="store_true", help="write only; print nothing")
    return p


def find(ctx: Context, name: str) -> tuple[review.Entry, str]:
    """The entry carrying the artifact and its current text, dying when there is not exactly one entry with one.

    A later round redrafts by appending a second `## artifact` block, so the last one in the file wins.
    The earlier drafts stay where they are as the record of what was shown and turned down, and are not what gets published.
    """
    paths, _ = ctx.open(name)
    holders = []
    for entry in ctx.entries(paths):
        blocks = [b for b in entry.blocks if b.type == ARTIFACT_BLOCK]
        if blocks:
            holders.append((entry, blocks[-1].prose.strip()))

    if not holders:
        ctx.die(
            f"no `## {ARTIFACT_BLOCK}` block in {name}. "
            f"The draft entry holds the text to publish — see tools/review/docs/entry-types/draft-artifact.md"
        )
    if len(holders) > 1:
        ctx.die(f"several entries carry an artifact block: {', '.join(e.slug for e, _ in holders)}")
    return holders[0]


def run(args: argparse.Namespace, ctx: Context) -> None:
    entry, text = find(ctx, args.name)

    if args.write:
        Path(args.write).write_text(text + "\n", encoding="utf-8")
        print(review.console.dim(f"wrote {args.write} from {entry.slug}"))

    if not args.quiet:
        print(text)
