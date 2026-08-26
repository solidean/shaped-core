"""`init` — create a review folder, pinned to a commit range and to at least one goal.

The base is resolved to the merge base and stored as a sha.
Re-resolving a three-dot range on every run would let the integration branch move the review's obligation underneath ids already handed out.

`init` is the one command that takes a `--repo`.
It writes what it resolved into `review.toml`, so every later command finds the checkout without being told —
which matters most for the usual shape, a branch checked out as a worktree somewhere else entirely.

A goal is mandatory because it decides what the review is *for* — a comment for someone else, changes landed in this session, or agreement on a design.
Those produce different entries and different end artifacts, so guessing one would be worse than asking.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import tools.review as review

from . import args as a
from .context import Context

NAME = "init"

_DEFAULT_BASES = ("origin/main", "origin/master", "main", "master")


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Start a review of a commit range")
    a.review_name(p)
    p.add_argument("--repo", default=None, metavar="PATH",
                   help="the checkout to review (default: the repository this runs from); recorded, so no later command needs it")
    p.add_argument("--range", dest="range_spec", metavar="A..B",
                   help="the range to review; defaults to the integration branch against HEAD")
    p.add_argument("--goal", action="append", default=[], metavar="GOAL",
                   help=f"repeatable, at least one required. {review.goal_help()}")
    p.add_argument("--title", default="", help="a human title for the review")
    p.add_argument("--context", type=int, default=8, help="diff context for the hunks a reader sees (default 8)")
    p.add_argument("--gap", type=int, default=20,
                   help="merge hunks closer than this many lines into one change (default 20; below 2*context it does nothing)")
    p.add_argument("--force", action="store_true", help="re-initialize an existing review folder")
    return p


def _seed_run_prefixes(repo: Path) -> list[str]:
    """What `review run` may execute here, guessed once so the common case needs no configuration.

    Empty by default and empty for a repository this does not recognise: running examples through `dev.py` is a
    fact about shaped-core, and this tool reviews any git repository.
    """
    return ["uv run dev.py example"] if (repo / "dev.py").is_file() else []


def _resolve_range(ctx: Context, spec: str | None) -> tuple[str, str, str, str]:
    """(base_sha, head_sha, base_spec, head_spec), with the merge base pinned."""
    if spec:
        base_spec, _, head_spec = spec.partition("..")
        head_spec = head_spec.lstrip(".").strip() or "HEAD"
        base_spec = base_spec.strip() or "HEAD"
    else:
        head_spec = "HEAD"
        base_spec = next((b for b in _DEFAULT_BASES if ctx.git.rev_parse(b)), "")
        if not base_spec:
            ctx.die("no integration branch found (tried origin/main, origin/master, main, master); pass --range A..B")

    try:
        base = ctx.git.require_rev(base_spec)
        head = ctx.git.require_rev(head_spec)
    except review.GitError as e:
        ctx.die(str(e))

    merge_base = ctx.git.merge_base(base, head)
    if merge_base is None:
        ctx.die(f"{base_spec} and {head_spec} have no common ancestor")
    return merge_base, head, base_spec, head_spec


def run(args: argparse.Namespace, ctx: Context) -> None:
    goals = [g.strip() for spec in args.goal for g in spec.split(",") if g.strip()]
    if not goals:
        ctx.die(f"--goal is required: what is this review for? {review.goal_help()}")
    for g in goals:
        if g not in review.GOALS:
            ctx.die(f"unknown goal {g!r}. Known goals: {', '.join(review.GOALS)}")

    if args.repo:
        ctx.target(Path(args.repo))

    paths = ctx.paths_for(args.name)
    if paths.exists() and not args.force:
        ctx.die(f"{ctx.rel(paths.root)} already holds a review; --force re-initializes it")

    design_only = goals == ["design"]
    base = head = base_spec = head_spec = ""
    if not design_only or args.range_spec:
        base, head, base_spec, head_spec = _resolve_range(ctx, args.range_spec)

    cfg = review.ReviewConfig(
        name=args.name, title=args.title, goals=goals,
        repo=ctx.record_repo(paths.root), upstream=ctx.git.remote_url(),
        base=base, head=head, base_spec=base_spec, head_spec=head_spec,
        context=args.context, coalesce_gap=args.gap, created=review.now(),
        run_prefixes=_seed_run_prefixes(ctx.repo),
    )

    paths.create()
    review.save(paths.config, cfg)
    review.record(paths.log, "init", goals=goals, base=base, head=head)
    ctx.warn_gitignore(paths)

    print(f"review {args.name} at {ctx.rel(paths.root)}")
    print(f"  goals   {', '.join(goals)}")
    print(f"  repo    {cfg.repo}")
    if base:
        commits = ctx.git.commits(base, head)
        print(f"  range   {base_spec}..{head_spec}  ({base[:12]}..{head[:12]})")
        print(f"  commits {len(commits)}")
        merges = ctx.git.has_merges(base, head)
        if merges:
            print(review.console.dim(
                f"  merges  {len(merges)} on the first-parent path; each counts as everything it brought in"
            ))
        print(f"\nnext: uv run review.py ingest {args.name}")
    else:
        print("  range   none (design review)")
        print(f"\nnext: write entries under {ctx.rel(paths.entries_dir)}")
