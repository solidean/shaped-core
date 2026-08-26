"""`post` — publish the review's artifact to the forge, once and deliberately.

This is the only command that leaves the machine, so it is the only one that asks twice.
It refuses without `--confirm`, and it refuses when the draft entry's own ask has not been answered —
the review already has a gate for "is this text right", and posting past it would make that gate decorative.

`gh` does the talking.
The PR number is required rather than inferred: a review pinned to `refs/pull/<n>/head` knows the branch it read, not the thread a comment belongs on.
Guessing that wrong publishes to a stranger's PR.
"""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path

import tools.review as review

from . import args as a
from . import artifact as artifact_cmd
from .context import Context

NAME = "post"

_TIMEOUT = 30


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Post the artifact to the PR as one comment")
    a.review_name(p)
    p.add_argument("--pr", type=int, required=True, metavar="N", help="the pull request number to comment on")
    p.add_argument("--confirm", action="store_true", help="actually post; without it this is a dry run")
    p.add_argument("--force", action="store_true", help="post even though the draft entry's ask is unanswered")
    return p


def _approved(ctx: Context, name: str, entry: review.Entry) -> tuple[bool, str]:
    """Whether the draft entry's ask carries a finalized answer, and what it said."""
    paths, _ = ctx.open(name)
    answers = ctx.answers(paths, entry)
    for block in entry.asks:
        answer = answers.get(block.name)
        if answer is None or answer.is_empty:
            return False, f"ask {block.name!r} has no answer"
        if answer.tentative:
            return False, f"ask {block.name!r} is answered but the round was never handed back"
        return True, "; ".join(answer.selected) or answer.text.strip()[:80]
    return False, "the draft entry has no ask"


def run(args: argparse.Namespace, ctx: Context) -> None:
    paths, cfg = ctx.open(args.name)
    entry, text = artifact_cmd.find(ctx, args.name)

    ok, detail = _approved(ctx, args.name, entry)
    if not ok and not args.force:
        ctx.die(f"{entry.slug}: {detail}. Answer it and hand the round back, or pass --force")

    print(f"review {cfg.name} → PR #{args.pr}")
    print(f"  source   {entry.slug} (`## {artifact_cmd.ARTIFACT_BLOCK}`)")
    print(f"  approval {detail}")
    print(f"  size     {len(text.splitlines())} lines, {len(text)} characters")

    if not args.confirm:
        print(review.console.yellow("\ndry run: nothing was posted. Add --confirm to publish it."))
        return

    with tempfile.TemporaryDirectory(prefix="review-post-") as tmp:
        body = Path(tmp) / "comment.md"
        body.write_text(text + "\n", encoding="utf-8")
        try:
            proc = subprocess.run(
                ["gh", "pr", "comment", str(args.pr), "--body-file", str(body)],
                cwd=str(ctx.repo), capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=_TIMEOUT,
            )
        except FileNotFoundError:
            ctx.die("gh is not on PATH, so there is nothing to post with")
        except (OSError, subprocess.TimeoutExpired) as e:
            ctx.die(f"`gh pr comment` could not run: {e}")

    if proc.returncode != 0:
        detail = (proc.stderr or proc.stdout or "").strip().splitlines()
        ctx.die(f"`gh pr comment` failed: {detail[0] if detail else f'exit {proc.returncode}'}")

    url = (proc.stdout or "").strip()
    review.record(paths.log, "post", pr=args.pr, entry=entry.slug, url=url)
    print(review.console.green(f"posted: {url}"))
