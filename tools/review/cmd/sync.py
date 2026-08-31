"""`sync` — move the review onto a branch that has moved.

Change ids are derived from content, so a hunk the author did not touch keeps the id it already had.
That is the whole reason `sync` can say anything useful: what comes back is not "everything changed" but the three
answers that matter — what is new, what is gone, and which entries were talking about what is gone.

A change whose claim no longer meets net space is **superseded**, never deleted.
An entry that discussed it stays readable, and in a `land-changes` review the fact that its change is superseded
is the evidence the fix actually landed.

A change that survives keeps its id and gets a new claim, since the id is content-derived and the claim is
positional — see `register` in lib/changeset/ingest.py.

The base is only re-pinned when asked for.
Silently following a moving integration branch would change the review's obligation underneath ids already handed out.
"""

from __future__ import annotations

import argparse

import tools.review as review

from . import args as a
from .context import Context

NAME = "sync"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Re-point the review at a moved head")
    a.review_name(p)
    p.add_argument("--head", default="", help="what to re-resolve head to (default: the spec init recorded)")
    p.add_argument("--rebase-base", action="store_true",
                   help="also re-pin the merge base, changing what the review is accountable for")
    p.add_argument("--dry-run", action="store_true", help="report what would change, write nothing")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open_changeset(args.name)

    head_spec = args.head or cfg.head_spec or cfg.head
    try:
        new_head = ctx.git.require_rev(head_spec)
        new_base = cfg.base
        if args.rebase_base:
            base_spec = cfg.base_spec or cfg.base
            merge_base = ctx.git.merge_base(ctx.git.require_rev(base_spec), new_head)
            if merge_base is None:
                ctx.die(f"{base_spec} and {head_spec} have no common ancestor")
            new_base = merge_base
    except review.GitError as e:
        ctx.die(str(e))

    if new_head == cfg.head and new_base == cfg.base:
        print(f"{cfg.name} is already at {cfg.head[:12]}; nothing moved")
        return

    print(f"head {cfg.head[:12]} -> {new_head[:12]}"
          + (f"   base {cfg.base[:12]} -> {new_base[:12]}" if new_base != cfg.base else ""))

    moved = review.ReviewConfig(**{**cfg.__dict__, "base": new_base, "head": new_head})
    net = ctx.net_space(moved)
    ledger = ctx.ledger(paths)

    gone = [c for c in ledger.live() if c.claim.intersect(net).is_empty]
    entries = ctx.entries(paths)
    stale = [
        (entry.id, entry.title, sorted(set(entry.referenced_changes()) & {c.id for c in gone}))
        for entry in entries
    ]
    stale = [(entry_id, title, ids) for entry_id, title, ids in stale if ids]

    candidates = review.candidates_for(
        ctx.git, new_base, new_head,
        context=moved.context, gap=moved.coalesce_gap, net=net,
    )
    known = ledger.by_digest()
    fresh = [c for c in candidates if c.digest not in known]

    print(f"{len(gone)} changes superseded, {len(fresh)} new changes to ingest")
    for entry_id, title, ids in stale:
        print(review.console.yellow(f"  entry {entry_id} {title} — references {', '.join(ids)}, now superseded"))

    if args.dry_run:
        print("(dry run: nothing written)")
        return

    for change in gone:
        change.superseded = True
        ledger.append(change)

    def write_body(change_id: str, text: str) -> None:
        review.write_atomic(paths.change_diff(change_id), text.rstrip("\n") + "\n")

    result = review.register(ledger, candidates, round_number=moved.next_round, write_body=write_body)

    cfg.base, cfg.head = new_base, new_head
    review.save(paths.config, cfg)
    review.record(paths.log, "sync", head=new_head, base=new_base,
                  superseded=len(gone), created=len(result.created), repointed=len(result.repointed))

    uncovered = net.subtract(ledger.covered())
    print(f"{len(result.created)} changes created")
    if result.repointed:
        # The common case on a head move: a hunk the author never touched, at different line numbers.
        # Its id is content-derived and therefore unchanged, so only the claim had to follow.
        print(f"{len(result.repointed)} claim(s) re-pointed at their new line numbers")
    if uncovered.is_empty:
        print(review.console.green(f"all {len(net)} atoms accounted for"))
    else:
        print(review.console.yellow(f"{len(uncovered)} atoms unaccounted — `review ingest {args.name} --rest`"))
    print(f"refresh the generated entries with `uv run review.py generate {args.name}`")
