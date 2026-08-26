"""`generate` — write or refresh the two entries the tool owns.

The overview and the coverage report are the only generated entries.
Both are refreshed in place: only blocks marked `generated:` are replaced, so commentary written under one survives every refresh.
"""

from __future__ import annotations

import argparse

import tools.review as review

from . import args as a
from .context import Context

NAME = "generate"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Write or refresh the overview and coverage entries")
    a.review_name(p)
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    paths.create()

    if not cfg.has_changeset:
        ctx.die("a design review has no changeset, so there is nothing to generate; write entries directly")

    net = ctx.net_space(cfg)
    ledger = ctx.ledger(paths)
    discharged = ctx.discharged(ctx.entries(paths))

    overview = paths.entries_dir / f"{review.OVERVIEW_SLUG}.md"
    review.ensure_entry(
        overview, review.overview_front(cfg), "overview",
        review.overview_body(ctx.git, cfg, net),
        title=review.OVERVIEW_TITLE,
    )

    coverage = paths.entries_dir / f"{review.COVERAGE_SLUG}.md"
    review.ensure_entry(
        coverage, review.coverage_front(), "coverage",
        review.coverage_body(cfg, net, ledger, discharged),
        title=review.COVERAGE_TITLE,
    )

    review.record(paths.log, "generate", entries=[review.OVERVIEW_SLUG, review.COVERAGE_SLUG])
    print(f"refreshed {ctx.rel(overview)}")
    print(f"refreshed {ctx.rel(coverage)}")
