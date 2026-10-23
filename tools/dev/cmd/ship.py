"""`ship` — pull, gate and push this repository and every submodule carrying unpushed work, in the one order that is safe.

A change that spans submodules lands as: each submodule's commits on its branch, the parent's gate over all of it,
the submodules pushed, the pointers bumped in a commit of their own, the parent pushed.
Done by hand, that sequence goes wrong in the obvious places, a submodule pushed before the gate ran or from the wrong directory,
so `ship` is the only command that pushes.

The gate is the project's own `dev.py check`, whatever registry the project wired, so its extra roots already cover the submodules' prose and links.
A push the remote rejected because it moved merges again and re-runs the static gates on top of what came in, up to three rounds;
the tests ran on this change already, and what came in was gated by whoever pushed it.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from tools import dev
from tools.dev import console

from ..lib.project import submodules as lib
from .context import Context

NAME = "ship"

# Rounds before a busy remote wins: each one pulls what came in and gates again.
_ROUNDS = 3


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Pull, gate and push this repository and every submodule with unpushed work, bumps included")
    p.add_argument("--bump", action="append", default=[], metavar="PATH",
                   help="Also bump this submodule, whose work is pushed already but whose pointer is not; repeatable")
    p.add_argument("--closes", metavar="ID[,ID]", default=None,
                   help="Close these issues in a commit of their own ahead of the gate, so the push that resolves them closes them; "
                        "nothing closes if the gate fails")
    p.add_argument("--resolution", choices=("fixed", "wontfix", "duplicate", "obsolete"), default="fixed",
                   help="How --closes closes them (default: fixed)")
    p.add_argument("--dry-run", action="store_true", help="Say what would be pulled, bumped and pushed, and change nothing")
    return p


def _git(repo: Path, *argv: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(repo), *argv], capture_output=True, text=True, encoding="utf-8", errors="replace")


def _say(text: str) -> None:
    dev.ui.write_line(console.dim(f"ship: {text}"))


def _dev(ctx: Context, *argv: str) -> int:
    """Run this project's own dev.py, so the project's registry and policy apply, not shaped-core's defaults."""
    return subprocess.run(["uv", "run", "dev.py", *argv], cwd=str(ctx.root)).returncode


def _branch_of(repo: Path) -> str | None:
    head = _git(repo, "symbolic-ref", "--quiet", "--short", "HEAD")
    return head.stdout.strip() if head.returncode == 0 else None


def _unpushed(repo: Path, branch: str) -> int:
    """Commits on HEAD that the remote's branch lacks; every commit when the branch was never pushed."""
    if _git(repo, "rev-parse", "--verify", "--quiet", f"origin/{branch}").returncode != 0:
        return int(_git(repo, "rev-list", "--count", "HEAD").stdout.strip() or "0")
    return int(_git(repo, "rev-list", "--count", f"origin/{branch}..HEAD").stdout.strip() or "0")


def _dirty(repo: Path) -> list[str]:
    """What is uncommitted in `repo`, a submodule's moved pointer aside: that is a bump, not an edit."""
    out = _git(repo, "status", "--porcelain", "--untracked-files=normal", "--ignore-submodules=all")
    return [line for line in out.stdout.splitlines() if line.strip()]


def _merge_upstream(ctx: Context, repo: Path, branch: str, label: str) -> None:
    fetched = _git(repo, "fetch", "origin", branch)
    if fetched.returncode != 0 and "couldn't find remote ref" not in fetched.stderr:
        ctx.die(f"ship: git fetch failed in {label}:\n{fetched.stderr.strip()}")
    if _git(repo, "rev-parse", "--verify", "--quiet", f"origin/{branch}").returncode != 0:
        return
    if _git(repo, "merge-base", "--is-ancestor", f"origin/{branch}", "HEAD").returncode == 0:
        return
    _say(f"merging origin/{branch} into {label}")
    merged = _git(repo, "merge", "--no-edit", f"origin/{branch}")
    if merged.returncode != 0:
        ctx.die(f"ship: the merge in {label} did not go through; resolve it, commit, and ship again:\n{(merged.stdout + merged.stderr).strip()}")


def _commit_closes(ctx: Context, ids: list[str], resolution: str) -> None:
    """Close each issue and commit the closes as one commit on top of HEAD, ahead of the gate, which validates them like any other."""
    for issue_id in ids:
        if _dev(ctx, "issues", "close", issue_id, "--resolution", resolution) != 0:
            ctx.die(f"ship: could not close {issue_id}")
    _git(ctx.root, "add", "-A")
    subject = f"issues: close {', '.join(ids)} ({resolution})"
    committed = _git(ctx.root, "commit", "-q", "-m", subject, "-m", "Resolved by the commits pushed with this one.")
    if committed.returncode != 0:
        ctx.die(f"ship: git commit failed after closing the issues:\n{(committed.stdout + committed.stderr).strip()}")
    _say(f"committed `{subject}`")


def _bump(ctx: Context, subs: list[lib.Submodule]) -> None:
    """Commit the moved pointers as one commit of their own, the presets the project derives from a submodule refreshed with them."""
    moved = [s for s in subs if lib.recorded(ctx.root, s) != _git(ctx.root / s.path, "rev-parse", "HEAD").stdout.strip()]
    if not moved:
        return
    paths = [s.path for s in moved]
    # A project whose registry derives a file from a submodule (CMakePresets.json from shaped-core's) refreshes it in the same commit.
    listed = subprocess.run(["uv", "run", "dev.py", "check", "--list"], cwd=str(ctx.root), capture_output=True, text=True)
    if "presets" in listed.stdout.split():
        _dev(ctx, "check", "presets", "--fix")
        paths += [line[3:] for line in _dirty(ctx.root)]
    _git(ctx.root, "add", "--", *paths)
    names = ", ".join(Path(s.path).name for s in moved)
    committed = _git(ctx.root, "commit", "-q", "-m", f"bump {names}", "--", *paths)
    if committed.returncode != 0:
        ctx.die(f"ship: the bump commit failed:\n{(committed.stdout + committed.stderr).strip()}")
    _say(f"committed the bump of {names}")


def _gate(ctx: Context, *, tests: bool) -> None:
    if _dev(ctx, "check", "--fix", *([] if tests else ["--no-test"])) != 0:
        ctx.die("ship: the gate failed; nothing was pushed (the bump commit, if any, stays local)")
    if _dirty(ctx.root):
        ctx.die("ship: the gate rewrote files; commit them and ship again")


def run(args: argparse.Namespace, ctx: Context) -> None:
    branch = _branch_of(ctx.root)
    if branch is None:
        ctx.die("ship needs a checked-out branch here")
    if dirty := _dirty(ctx.root):
        ctx.die("ship needs every edit committed; uncommitted here:\n  " + "\n  ".join(dirty[:10]))

    # The submodules that carry work of their own: unpushed commits on the branch .gitmodules names.
    subs = lib.list_submodules(ctx.root)
    named = {Path(p).as_posix().rstrip("/") for p in args.bump}
    shipping: list[lib.Submodule] = []
    bumping: list[lib.Submodule] = []
    for s in subs:
        repo = ctx.root / s.path
        if dirty := _dirty(repo):
            ctx.die(f"ship needs every edit committed; uncommitted in {s.path}:\n  " + "\n  ".join(dirty[:10]))
        if s.branch is None:
            continue
        _git(repo, "fetch", "origin", s.branch)
        ahead = _unpushed(repo, s.branch) if _branch_of(repo) == s.branch else 0
        if _branch_of(repo) != s.branch and _git(repo, "branch", "--contains", "HEAD", "-r").stdout.strip() == "":
            ctx.die(f"ship: {s.path} holds commits on no branch; check out {s.branch} there first")
        if ahead:
            shipping.append(s)
        if ahead or s.path in named:
            bumping.append(s)
    unknown = named - {s.path for s in subs}
    if unknown:
        ctx.die(f"ship --bump: not a submodule: {', '.join(sorted(unknown))}")

    _say(f"submodules with unpushed work: {', '.join(s.path for s in shipping) or 'none'}")
    if args.dry_run:
        _say(f"would bump: {', '.join(s.path for s in bumping) or 'nothing'}; would push {branch} and those submodules")
        return

    for s in shipping:
        _merge_upstream(ctx, ctx.root / s.path, s.branch, s.path)
    _merge_upstream(ctx, ctx.root, branch, "this repository")
    _dev(ctx, "submodules", "sync")  # brings the others to what a pull recorded; never touches the ones ahead

    # A submodule with a driver of its own has a gate of its own, which its rules require before its push; the parent's gate does not run it.
    for s in shipping:
        repo = ctx.root / s.path
        if not (repo / "dev.py").is_file():
            continue
        _say(f"gating {s.path} with its own check")
        if subprocess.run(["uv", "run", "dev.py", "check", "--fix"], cwd=str(repo)).returncode != 0:
            ctx.die(f"ship: {s.path}'s own gate failed; nothing was pushed")
        if _dirty(repo):
            ctx.die(f"ship: {s.path}'s gate rewrote files; commit them there and ship again")

    if args.closes:
        _commit_closes(ctx, [i.strip() for i in args.closes.split(",") if i.strip()], args.resolution)
    _bump(ctx, bumping)

    for round_no in range(1, _ROUNDS + 1):
        # A later round follows a merge the remote forced: the static gates see what it broke, and the tests ran on this change already.
        _gate(ctx, tests=round_no == 1)
        # Submodules first, so the pointers the parent pushes always name commits their remotes have.
        for s in shipping:
            pushed = _git(ctx.root / s.path, "push", "origin", f"HEAD:{s.branch}")
            if pushed.returncode != 0:
                ctx.die(f"ship: pushing {s.path} failed; its remote moved, so ship again to merge it:\n{pushed.stderr.strip()}")
            _say(f"pushed {s.path} to origin/{s.branch}")
        shipping = []
        pushed = _git(ctx.root, "push", "origin", branch)
        if pushed.returncode == 0:
            dev.ui.write_line(console.green(f"ship: pushed {branch} to origin"))
            return
        text = pushed.stdout + pushed.stderr
        if "fetch first" not in text and "non-fast-forward" not in text and "rejected" not in text:
            ctx.die(f"ship: git push failed:\n{text.strip()}")
        _say(f"origin/{branch} moved during the gate (round {round_no}); merging it and gating again")
        _merge_upstream(ctx, ctx.root, branch, "this repository")
        _dev(ctx, "submodules", "sync")
    ctx.die(f"ship: origin/{branch} moved {_ROUNDS} times while gating; it is busy, ship again in a moment")
