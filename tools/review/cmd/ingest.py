"""`ingest` — give every change in the range an identity.

The default sweep covers the whole obligation by construction, so one run turns the coverage gate green.
The flags exist for the cases where reading every hunk is the wrong use of attention: a vendored drop, a mechanical sweep,
or a second pass that should only pick up what a previous, narrower ingest left behind.
"""

from __future__ import annotations

import argparse
import fnmatch

import tools.review as review

from . import args as a
from .context import Context

NAME = "ingest"

_PREVIEW = 40


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Give change ids to the range, or to part of it")
    a.review_name(p)
    p.add_argument("--paths", action="append", default=[], metavar="SEL",
                   help="restrict to these paths (repeatable, comma-separated)")
    p.add_argument("--commits", metavar="A..B",
                   help="take hunks from these commits individually, instead of from the net diff")
    p.add_argument("--bulk", metavar="SEL",
                   help="one change covering everything under SEL, with no hunk bodies written")
    p.add_argument("--bulk-commits", metavar="A..B",
                   help="one change covering everything these commits contributed; combines with --bulk and --paths")
    p.add_argument("--reason", default="",
                   help="why the bulk is not being read hunk by hunk; required with either --bulk form")
    p.add_argument("--rest", action="store_true",
                   help="only create changes for atoms nothing claims yet")
    p.add_argument("--dry-run", action="store_true", help="report what would be created, write nothing")
    p.add_argument("--stats", action="store_true", help="report the shape of the change set and stop")
    return p


def _split(values: list[str]) -> list[str]:
    return [v.strip() for spec in values for v in spec.split(",") if v.strip()]


def matcher(selector: str):
    """Match a path against a comma-separated selector: a trailing slash means a subtree, otherwise it is a glob."""
    terms = _split([selector])

    def matches(path: str) -> bool:
        for term in terms:
            if term.endswith("/"):
                if path.startswith(term):
                    return True
            elif fnmatch.fnmatch(path, term) or path == term:
                return True
        return False

    return matches


def _resolve_commits(ctx: Context, cfg: review.ReviewConfig, spec: str) -> list:
    """The commits a `--commits` / `--bulk-commits` spec selects, along the first-parent path.

    A merge counts as the work it brought in, which is this repo's convention for naming a single commit
    and the only reading under which "bulk that merge" means anything.
    A single sha selects just that commit.
    """
    if ".." in spec:
        spec_base, _, spec_head = spec.partition("..")
        spec_head = spec_head.lstrip(".").strip() or cfg.head
        spec_base = spec_base.strip() or cfg.base
    else:
        spec_head = spec.strip()
        spec_base = spec_head + "^"

    try:
        base_sha = ctx.git.require_rev(spec_base)
        head_sha = ctx.git.require_rev(spec_head)
        commits = ctx.git.commits(base_sha, head_sha)
        merges = ctx.git.has_merges(base_sha, head_sha)
    except review.GitError as e:
        ctx.die(str(e))

    if not commits:
        ctx.die(f"{spec} selects no commits")

    # A commit outside the review's own range contributes nothing, and saying that plainly beats reporting an empty claim.
    try:
        in_range = {c.sha for c in ctx.git.commits(cfg.base, cfg.head)}
    except review.GitError:
        in_range = set()
    outside = [c for c in commits if in_range and c.sha not in in_range]
    if outside and len(outside) == len(commits):
        ctx.die(
            f"{spec} selects no commit inside this review's range ({cfg.base_spec}..{cfg.head_spec}); "
            f"{outside[0].short} is not one of the {len(in_range)} commits being reviewed"
        )
    if outside:
        print(review.console.yellow(
            f"{len(outside)} of {len(commits)} commits are outside the review's range and contribute nothing"
        ))
    if merges:
        print(review.console.dim(
            f"{len(merges)} merge commit(s) in {spec}; each counts as everything it brought in"
        ))
    return commits


def _commit_candidates(ctx: Context, cfg: review.ReviewConfig, net: review.LineSpace, args) -> list:
    """Hunks taken from individual commits rather than from the net diff."""
    commits = _resolve_commits(ctx, cfg, args.commits)
    selectors = _split(args.paths)
    found, notes = review.collect_commit_candidates(
        ctx.git, [c.sha for c in commits],
        base=cfg.base, head=cfg.head, context=cfg.context, gap=cfg.coalesce_gap,
        net=net, paths=selectors or None,
    )
    for note in notes:
        print(review.console.dim(note))
    return found


def _bulk_commit_candidate(ctx: Context, cfg: review.ReviewConfig, net: review.LineSpace, args):
    """One claim over everything a set of commits contributed, narrowed by --bulk / --paths where given."""
    commits = _resolve_commits(ctx, cfg, args.bulk_commits)
    selectors = _split(args.paths)
    label = args.bulk_commits + (f" ∩ {args.bulk}" if args.bulk else "")

    return review.bulk_candidate_for_commits(
        ctx.git, [c.sha for c in commits],
        base=cfg.base, head=cfg.head, net=net,
        reason=args.reason, label=label,
        matches=matcher(args.bulk) if args.bulk else None,
        paths=selectors or None,
    )


def _print_stats(cfg: review.ReviewConfig, net: review.LineSpace, candidates: list) -> None:
    by_path: dict[str, int] = {}
    for candidate in candidates:
        by_path[candidate.path] = by_path.get(candidate.path, 0) + 1

    print(f"{len(net)} atoms across {len(net.paths())} files; {len(candidates)} changes would be created")
    print(f"context {cfg.context}, coalesce gap {cfg.coalesce_gap}")
    print()
    print(f"{'changes':>8}  {'atoms':>7}  path")
    for path in sorted(by_path, key=lambda p: (-by_path[p], p)):
        print(f"{by_path[path]:>8}  {len(net.for_path(path)):>7}  {path}")


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open_changeset(args.name)
    bulking = bool(args.bulk or args.bulk_commits)
    if bulking and not args.reason:
        ctx.die("a bulk claim requires --reason: say why these changes are not being read hunk by hunk")
    if args.commits and args.bulk_commits:
        ctx.die("--commits and --bulk-commits are two different answers to the same question; pick one")

    net = ctx.net_space(cfg)
    ledger = ctx.ledger(paths)

    if args.bulk_commits:
        candidate = _bulk_commit_candidate(ctx, cfg, net, args)
        if candidate is None:
            ctx.die(f"--bulk-commits {args.bulk_commits!r} claims nothing that still stands at head")
        candidates = [candidate]
    elif args.commits:
        candidates = _commit_candidates(ctx, cfg, net, args)
    elif args.bulk:
        candidate = review.bulk_candidate(net, selector=args.bulk, reason=args.reason, matches=matcher(args.bulk))
        if candidate is None:
            ctx.die(f"--bulk {args.bulk!r} matches nothing in this range")
        candidates = [candidate]
    else:
        selectors = _split(args.paths)
        candidates = review.candidates_for(
            ctx.git, cfg.base, cfg.head,
            context=cfg.context, gap=cfg.coalesce_gap, net=net, paths=selectors or None,
        )

    if args.stats:
        _print_stats(cfg, net, candidates)
        return

    if args.dry_run:
        known = ledger.by_digest()
        fresh = [c for c in candidates if c.digest not in known]
        print(f"{len(candidates)} candidates, {len(fresh)} new, {len(candidates) - len(fresh)} already in the ledger")
        for candidate in fresh[:_PREVIEW]:
            print(f"  {candidate.kind:<11} {candidate.summary}")
        if len(fresh) > _PREVIEW:
            print(f"  ... and {len(fresh) - _PREVIEW} more")
        return

    def write_body(change_id: str, text: str) -> None:
        review.write_atomic(paths.change_diff(change_id), text.rstrip("\n") + "\n")

    result = review.register(
        ledger, candidates, round_number=cfg.next_round,
        write_body=write_body, only_uncovered=args.rest,
    )
    review.record(
        paths.log, "ingest", created=len(result.created), reused=len(result.reused), repointed=len(result.repointed),
        bulk=args.bulk or "", rest=args.rest,
    )

    print(f"{len(result.created)} changes created, {len(result.reused)} already known")
    if result.repointed:
        # A head move leaves a claim in the old coordinates; saying so is what keeps a changed count explicable.
        print(f"{len(result.repointed)} claim(s) re-pointed at their new line numbers")
    if result.skipped_covered:
        print(f"{result.skipped_covered} skipped as already covered (--rest)")

    uncovered = net.subtract(ledger.covered())
    if uncovered.is_empty:
        print(review.console.green(f"all {len(net)} atoms accounted for"))
    else:
        print(review.console.yellow(
            f"{len(uncovered)} of {len(net)} atoms still unaccounted - `review coverage {args.name}` lists them"
        ))
