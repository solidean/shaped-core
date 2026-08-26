"""`title` — name a review after reading it, rather than before.

`init --title` asks for a name at the worst possible moment: nobody has read the range yet.
By the time the agent has written the entries it knows what the review is about, and a tab called
"the bindless split and what it costs" is findable in a way `pr-146` is not.

`review.toml` is hand-editable, which is the escape hatch rather than the path — routing a normal operation
through hand-editing makes the escape hatch the interface.
"""

from __future__ import annotations

import argparse

import tools.review as review

from . import args as a
from .context import Context

NAME = "title"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Set the review's human title, shown in the tab and the navigation")
    a.review_name(p)
    p.add_argument("text", nargs="?", default="", help="the title; omit to print the current one")
    p.add_argument("--clear", action="store_true", help="drop the title and fall back to the review's name")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)

    if not args.text and not args.clear:
        print(cfg.title or review.console.dim(f"(none; the page shows {cfg.name})"))
        return

    cfg.title = "" if args.clear else args.text.strip()
    review.save(paths.config, cfg)
    review.record(paths.log, "title", title=cfg.title)
    print(f"{cfg.name}: {cfg.title or 'no title; the page falls back to the name'}")
