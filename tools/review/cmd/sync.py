"""`sync` — move the review onto a branch that has moved.

Change ids are derived from content, so a hunk the author did not touch keeps the id it already had.
That is the whole reason `sync` can say anything useful: what comes back is not "everything changed" but the three
answers that matter — what is new, what is gone, and which entries were talking about what is gone.

A change whose content is gone from the range is **superseded**, never deleted.
An entry that discussed it stays readable, and in a `land-changes` review the fact that its change is superseded
is the evidence the fix actually landed.
A hunk edited in place is gone in that sense too.
The hunk replacing it records its id, which the page takes the interdiff against and the warning about an entry names.

A change that survives keeps its id and gets a new claim, since the id is content-derived and the claim is
positional — see `register` in lib/changeset/ingest.py.
A superseded change whose content comes back, as an edit reverted, is made live again, since no ingest re-creates it.

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

    # Before the head moves, since a round with no recorded head was read at one this review has stood at.
    # Also where a review started by an older tool gets its early rounds' heads back, so it runs even when nothing moved.
    backfilled = not args.dry_run and cfg.backfill_round_heads(review.heads_at_finalize(paths.log))
    if backfilled:
        review.save(paths.config, cfg)
        print("recovered the heads earlier rounds were read at, from the log")

    if new_head == cfg.head and new_base == cfg.base:
        print(f"{cfg.name} is already at {cfg.head[:12]}; nothing moved")
        return

    print(f"head {cfg.head[:12]} -> {new_head[:12]}"
          + (f"   base {cfg.base[:12]} -> {new_base[:12]}" if new_base != cfg.base else ""))

    moved = review.ReviewConfig(**{**cfg.__dict__, "base": new_base, "head": new_head})
    net = ctx.net_space(moved)
    ledger = ctx.ledger(paths)

    candidates = review.candidates_for(
        ctx.git, new_base, new_head,
        context=moved.context, gap=moved.coalesce_gap, net=net,
    )
    gone = review.superseded_by_move(ledger, candidates, net)
    returned = review.returned_by_move(ledger, candidates)
    entries = ctx.entries(paths)

    known = ledger.by_digest()
    fresh = [c for c in candidates if c.digest not in known]
    returned_digests = {c.digest for c in returned}
    returning = {c.digest: c for c in candidates if c.digest in returned_digests}
    review.link_successors(
        [*fresh, *returning.values()], gone,
        head_map=_line_map(ctx, cfg.head, new_head), base_map=_line_map(ctx, cfg.base, new_base),
    )
    successors: dict[str, list[review.Candidate]] = {}
    for candidate in candidates:
        for old_id in candidate.supersedes:
            successors.setdefault(old_id, []).append(candidate)

    edited = f" ({len(successors)} edited in place)" if successors else ""
    print(f"{len(gone)} changes superseded{edited}, {len(fresh)} new changes to ingest")
    if returned:
        print(f"{len(returned)} superseded change(s) back at the new head, live again")

    if args.dry_run:
        _warn_stale(entries, gone, successors, known)
        print("(dry run: nothing written)")
        return

    for change in gone:
        change.superseded = True
        ledger.append(change)
    for change in returned:
        change.superseded = False
        change.supersedes = returning[change.digest].supersedes or change.supersedes
        ledger.append(change)

    def write_body(change_id: str, text: str) -> None:
        review.write_atomic(paths.change_diff(change_id), text.rstrip("\n") + "\n")

    result = review.register(ledger, candidates, round_number=moved.next_round, write_body=write_body)
    _warn_stale(entries, gone, successors, ledger.by_digest())

    cfg.base, cfg.head = new_base, new_head
    review.save(paths.config, cfg)
    review.record(paths.log, "sync", head=new_head, base=new_base,
                  superseded=len(gone), edited=len(successors), revived=len(returned),
                  created=len(result.created), repointed=len(result.repointed))

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
    print(f"refresh the generated entries with `{ctx.invocation} generate {args.name}`")


def _line_map(ctx: Context, earlier: str, later: str) -> review.TreeMap:
    """How lines moved from one commit to another; the identity where nothing moved or the earlier one is gone."""
    if earlier == later:
        return review.TreeMap()
    try:
        return review.build_line_map(ctx.git, earlier, later)
    except review.GitError:
        # A force-push can leave the old head unreachable and collected, and then successors are found on raw numbers.
        return review.TreeMap()


def _warn_stale(entries: list[review.Entry], gone: list[review.Change],
                successors: dict[str, list[review.Candidate]], ids: dict[str, str]) -> None:
    """Name every entry that references a change this sync retires, and what replaced it.

    `ids` maps a digest to its change id, so a dry run, which allocates none, names a new successor by its summary.
    """
    retired = {c.id for c in gone}
    for entry in entries:
        hits = sorted(set(entry.referenced_changes()) & retired)
        if not hits:
            continue
        named = []
        for change_id in hits:
            after = ", ".join(ids.get(c.digest) or c.summary for c in successors.get(change_id, []))
            named.append(f"{change_id} (replaced by {after})" if after else change_id)
        warning = f"  entry {entry.id} {entry.title} — references {', '.join(named)}, now superseded"
        print(review.console.yellow(warning))
