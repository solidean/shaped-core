"""`list` — the reviews this repository has, and where each one stands.

A review is scratch space that outlives a session, so coming back days later needs a way to find it again.
"""

from __future__ import annotations

import argparse

import tools.review as review

from .context import Context

NAME = "list"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="List the reviews in this repository")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    root = ctx.dir_override.parent if ctx.dir_override is not None else review.reviews_root(ctx.repo)
    if not root.is_dir():
        print(f"no reviews under {ctx.rel(root)}")
        return

    rows = []
    for folder in sorted(p for p in root.iterdir() if p.is_dir()):
        paths = review.ReviewPaths(folder)
        if not paths.exists():
            continue
        try:
            cfg = review.load(paths.config)
        except review.ConfigError:
            rows.append((folder.name, "unreadable", "", "", ""))
            continue

        entries = len(paths.entry_files())
        changes = len(review.Ledger.load(paths.ledger).live()) if cfg.has_changeset else 0
        rows.append((
            cfg.name,
            ", ".join(cfg.goals),
            f"round {cfg.next_round}",
            f"{entries} entries",
            f"{changes} changes" if cfg.has_changeset else "design",
        ))

    if not rows:
        print(f"no reviews under {ctx.rel(root)}")
        return

    width = max(len(r[0]) for r in rows)
    for name, goals, rnd, entries, changes in rows:
        print(f"{name:<{width}}  {rnd:<9}  {entries:<12}  {changes:<14}  {goals}")
