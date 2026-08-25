"""`finalize` — assemble the review's end artifact.

Which artifact depends on the goal, and the result is a draft in every case.
The tool can gather what was decided; deciding which points are worth someone's afternoon is the agent's job, with the maintainer.
"""

from __future__ import annotations

import argparse

import tools.review as review

from . import args as a
from .context import Context

NAME = "finalize"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Assemble the review's end artifact")
    a.review_name(p)
    p.add_argument("--goal", default="", help="which artifact to build, when the review has several goals")
    p.add_argument("--write", action="store_true", help="also write it into the review folder")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    goal = args.goal or review.finalizer_for(cfg.goals)
    if goal not in review.ARTIFACTS:
        ctx.die(f"unknown goal {goal!r}. Known goals: {', '.join(review.ARTIFACTS)}")
    if args.goal and args.goal not in cfg.goals:
        print(review.console.yellow(f"note: {goal!r} is not one of this review's goals ({', '.join(cfg.goals)})"))

    entries = ctx.entries(paths)
    pairs = [(entry, ctx.answers(paths, entry)) for entry in entries]
    text = review.ARTIFACTS[goal](cfg, pairs)

    if args.write:
        target = paths.root / f"artifact-{goal}.md"
        review.write_atomic(target, text)
        review.record(paths.log, "finalize-artifact", goal=goal, path=str(target))
        print(review.console.dim(f"wrote {ctx.rel(target)}"))
    print(text)
