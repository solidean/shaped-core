"""Companion repositories: opt-in clones the project keeps at a gitignored path inside its tree, recorded nowhere in its history.

A companion is what a submodule would be if most people never wanted it — a corpus, a large data set — so nothing about it is pinned.
It is cloned on request, works on its own branch with its own pushes, and moves only by a fast-forward to that branch's head.
Syncing follows the submodule rules (lib/project/submodules.py): uncommitted changes, another branch or unpushed commits are reported and left alone.

The path must be gitignored before anything is cloned there, so a companion can never make the project's tree dirty.
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path

from . import submodules
from .submodules import Outcome


@dataclass(frozen=True)
class Companion:
    name: str
    url: str
    # Relative to the project root, and gitignored there.
    path: str
    branch: str = "main"
    # One line for `companions list`: who wants this clone.
    purpose: str = ""


def _git(repo: Path, *argv: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(repo), *argv], capture_output=True, text=True, encoding="utf-8", errors="replace")


def is_cloned(root: Path, c: Companion) -> bool:
    return (root / c.path / ".git").exists()


def is_ignored(root: Path, c: Companion) -> bool:
    # A trailing slash asks about the directory itself, so the answer holds before it exists.
    return _git(root, "check-ignore", "-q", "--no-index", f"{c.path}/").returncode == 0


def init(root: Path, c: Companion, *, url: str | None = None, dry_run: bool = False) -> Outcome:
    """Clone the companion on its branch; one already cloned is reported, never touched."""
    if is_cloned(root, c):
        return Outcome(c.path, "up-to-date", "already cloned; `sync` fast-forwards it")
    if not is_ignored(root, c):
        return Outcome(c.path, "refused", "the path is not gitignored, so a clone there would dirty the tree")
    target = root / c.path
    if target.exists() and any(target.iterdir()):
        return Outcome(c.path, "refused", "the path exists and is not empty")
    source = url or c.url
    if dry_run:
        return Outcome(c.path, "would init", f"clone {source} on {c.branch}")
    target.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run(["git", "clone", "-q", "--branch", c.branch, source, str(target)],
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        return Outcome(c.path, "refused", f"git clone: {(proc.stderr or proc.stdout).strip()}")
    head = _git(target, "rev-parse", "HEAD").stdout.strip()
    return Outcome(c.path, "initialized", f"{c.branch} at {head[:10]} from {source}", head, head)


def sync(root: Path, c: Companion, *, dry_run: bool = False, fetch: bool = True) -> Outcome:
    """Fast-forward a cloned companion to the head of its branch, or say why not."""
    if not is_cloned(root, c):
        return Outcome(c.path, "skipped", f"not cloned; `companions init {c.name}` clones it")
    return submodules.sync_branch(root / c.path, c.path, c.branch, dry_run=dry_run, fetch=fetch)


def describe(root: Path, c: Companion) -> str:
    """Where a clone stands, without touching the network: its branch and commit, and whether it carries local work."""
    if not is_cloned(root, c):
        return "not cloned"
    repo = root / c.path
    head = _git(repo, "rev-parse", "--short=10", "HEAD").stdout.strip()
    on = _git(repo, "symbolic-ref", "-q", "--short", "HEAD").stdout.strip() or "detached"
    parts = [f"{on} at {head}"]
    if _git(repo, "status", "--porcelain").stdout.strip():
        parts.append("uncommitted changes")
    counts = _git(repo, "rev-list", "--left-right", "--count", f"HEAD...origin/{c.branch}").stdout.split()
    if len(counts) == 2:
        ahead, behind = int(counts[0]), int(counts[1])
        if ahead:
            parts.append(f"{ahead} unpushed commit(s)")
        if behind:
            parts.append(f"{behind} commit(s) behind origin/{c.branch} as last fetched")
    return ", ".join(parts)
