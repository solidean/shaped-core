"""`coverage` — what the review has accounted for, and what it has not.

Two questions, deliberately separate.
**Is every change identified** is a gate: an atom nobody gave an id to is invisible, not merely unreviewed.
**Is every change discharged** is progress: an id no entry references is work still to do, and the review is simply unfinished.
"""

from __future__ import annotations

import argparse
import json

import tools.review as review

from . import args as a
from .context import Context

NAME = "coverage"

_MAX_RUNS = 40


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Report what the review has accounted for")
    a.review_name(p)
    a.as_json(p)
    p.add_argument("--full", action="store_true", help="list every uncovered run, not the first few")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    if not cfg.has_changeset:
        print("design review: no changeset, so nothing to account for")
        return

    ctx.open_changeset(args.name)
    net = ctx.net_space(cfg)
    ledger = ctx.ledger(paths)
    uncovered = net.subtract(ledger.covered())

    live = ledger.live()
    bulk_ids = {c.id for c in live if c.discharged_by_reason}

    entries = ctx.entries(paths)
    thin = ctx.thinly_discharged(entries)

    if args.json:
        print(json.dumps({
            "atoms": len(net),
            "uncovered": len(uncovered),
            "changes": len(live),
            "bulk": len(bulk_ids),
            "runs": [{"path": p, "side": s, "start": lo, "end": hi} for p, s, lo, hi in uncovered.runs()],
            # The human report lists these under the runs, so a script reading the JSON must see them too:
            # an uncovered count with an empty `runs` reads as a bug in the reader rather than a file atom nobody claimed.
            "files": [{"path": a.path, "kind": a.kind, "discriminant": a.discriminant}
                      for a in sorted(uncovered.files, key=lambda a: (a.path, a.kind))],
            "thin": [{"change": cid, "entries": slugs} for cid, slugs in sorted(thin.items())],
        }, indent=2))
        return

    print(f"{len(net) - len(uncovered)}/{len(net)} atoms accounted for by {len(live)} changes")
    if bulk_ids:
        print(f"{len(bulk_ids)} bulk claims carry their own reason")

    if thin:
        # Accounted for and not read is the failure this names, and it is invisible in both gates.
        print(review.console.yellow(
            f"{len(thin)} change(s) discharged only by a meta or orientation entry, so nothing reviewed their contents"
        ))
        for change_id, slugs in sorted(thin.items()):
            change = ledger.resolve(change_id)
            where = change.summary or change.path if change is not None else change_id
            print(f"  {change_id}  {where}  ({', '.join(slugs)})")

    if uncovered.is_empty:
        print(review.console.green("gate 1 green: every change in the range has an identity"))
        return

    runs = uncovered.runs()
    print(review.console.yellow(f"gate 1 red: {len(uncovered)} atoms with no change id"))
    shown = runs if args.full else runs[:_MAX_RUNS]
    for path, side, lo, hi in shown:
        span = f"{lo}" if lo == hi else f"{lo}-{hi}"
        print(f"  {path}:{span} ({side})")
    if len(runs) > len(shown):
        print(f"  ... and {len(runs) - len(shown)} more runs; --full lists them")
    for atom in sorted(uncovered.files, key=lambda a: (a.path, a.kind)):
        print(f"  {atom.describe()}")
    print(f"`uv run review.py ingest {args.name} --rest` gives them ids")
