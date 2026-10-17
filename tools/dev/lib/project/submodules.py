"""Bring each submodule up to date with the branch `.gitmodules` names for it, never at the cost of local work.

Only two states are moved, and both by a fast-forward alone:
- the submodule is on its tracked branch, with nothing uncommitted and nothing unpushed;
- its HEAD is detached at a commit the tracked branch already contains, which is how `git submodule update` leaves it.
Everything else is reported and left alone: uncommitted changes, unpushed commits, another branch checked out, a detached commit the branch does not contain.
So nothing is ever reset, rebased, merged or rewritten, and no commit can be stranded.
A submodule `.gitmodules` declares but that is not checked out yet, say one a pull just added, is initialized first at the recorded commit, which leaves it detached within its branch.
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Submodule:
    path: str
    name: str
    # `.gitmodules`' `branch =`; None when it names none.
    branch: str | None


@dataclass(frozen=True)
class Outcome:
    path: str
    # "initialized", "updated", "up-to-date", "skipped" or "refused"; dry runs say "would init" or "would update".
    status: str
    detail: str
    old: str | None = None
    new: str | None = None

    @property
    def ok(self) -> bool:
        return self.status != "refused"


class _GitError(Exception):
    pass


def _git(root: Path, *argv: str, check: bool = True) -> subprocess.CompletedProcess:
    proc = subprocess.run(["git", "-C", str(root), *argv], capture_output=True, text=True, encoding="utf-8",
                          errors="replace")
    if check and proc.returncode != 0:
        raise _GitError(f"git {' '.join(argv)}: {(proc.stderr or proc.stdout).strip()}")
    return proc


def _out(root: Path, *argv: str) -> str:
    return _git(root, *argv).stdout.strip()


def _count(root: Path, revs: str) -> int:
    return int(_out(root, "rev-list", "--count", revs))


def list_submodules(root: Path, *, missing: bool = False) -> list[Submodule]:
    """The checked-out submodules `.gitmodules` declares, in its order; with `missing`, only those not checked out."""
    if not (root / ".gitmodules").is_file():
        return []
    listed = _git(root, "config", "-f", ".gitmodules", "--get-regexp", r"^submodule\..*\.path$", check=False).stdout
    subs = []
    for line in listed.splitlines():
        key, path = line.split(" ", 1)
        name = key[len("submodule."):-len(".path")]
        if (root / path / ".git").exists() == missing:
            continue
        branch = _git(root, "config", "-f", ".gitmodules", f"submodule.{name}.branch", check=False).stdout.strip()
        subs.append(Submodule(path, name, branch or None))
    return subs


def init(root: Path, sub: Submodule, *, dry_run: bool = False) -> Outcome:
    """Check a declared submodule out at the commit the superproject records, as `git submodule update --init` does."""
    at = recorded(root, sub)
    detail = f"at the recorded {at[:10]}" if at else "at the recorded commit"
    if dry_run:
        return Outcome(sub.path, "would init", detail)
    try:
        _git(root, "submodule", "update", "--init", "--recursive", "--", sub.path)
    except _GitError as e:
        return Outcome(sub.path, "refused", str(e))
    return Outcome(sub.path, "initialized", detail, at, at)


def sync(root: Path, sub: Submodule, *, dry_run: bool = False, fetch: bool = True) -> Outcome:
    """Fast-forward one submodule to its tracked branch, or say why not."""
    repo = root / sub.path
    try:
        return _sync(repo, sub, dry_run=dry_run, fetch=fetch)
    except _GitError as e:
        return Outcome(sub.path, "refused", str(e))


def _sync(repo: Path, sub: Submodule, *, dry_run: bool, fetch: bool) -> Outcome:
    if sub.branch is None:
        return Outcome(sub.path, "skipped", "no `branch =` in .gitmodules")
    branch = sub.branch
    remote = _git(repo, "config", f"branch.{branch}.remote", check=False).stdout.strip() or "origin"
    tracking = f"refs/remotes/{remote}/{branch}"

    # Untracked files are not judged: a fast-forward never touches one, and git refuses rather than overwrite it.
    if _out(repo, "status", "--porcelain", "--untracked-files=no"):
        return Outcome(sub.path, "refused", "uncommitted changes")
    if _git(repo, "rev-parse", "-q", "--verify", "MERGE_HEAD", check=False).returncode == 0:
        return Outcome(sub.path, "refused", "a merge is in progress")
    if fetch:
        _git(repo, "fetch", "-q", remote, f"+refs/heads/{branch}:{tracking}")
    target = _out(repo, "rev-parse", "--verify", f"{tracking}^{{commit}}")
    head = _out(repo, "rev-parse", "HEAD")
    on = _git(repo, "symbolic-ref", "-q", "--short", "HEAD", check=False).stdout.strip()

    if on and on != branch:
        return Outcome(sub.path, "skipped", f"{on} is checked out, and it tracks {branch}")
    if on:
        ahead = _count(repo, f"{target}..HEAD")
        if ahead:
            return Outcome(sub.path, "refused", f"{ahead} unpushed commit(s) on {branch}")
    else:
        if _git(repo, "merge-base", "--is-ancestor", "HEAD", target, check=False).returncode != 0:
            return Outcome(sub.path, "refused", f"detached at {head[:10]}, which {remote}/{branch} does not contain")
        local = _git(repo, "rev-parse", "-q", "--verify", f"refs/heads/{branch}", check=False).stdout.strip()
        if local and _count(repo, f"{target}..{local}"):
            return Outcome(sub.path, "refused", f"local {branch} has {_count(repo, f'{target}..{local}')} unpushed commit(s)")

    behind = _count(repo, f"HEAD..{target}")
    if on and not behind:
        return Outcome(sub.path, "up-to-date", f"{branch} at {head[:10]}", head, head)
    if on:
        detail = f"{branch}: {head[:10]}..{target[:10]}, {behind} commit(s)"
    else:
        detail = f"checks {branch} out at the detached {head[:10]}" + (f", then {behind} commit(s) to {target[:10]}" if behind else "")
    if dry_run:
        return Outcome(sub.path, "would update", detail, head, target)

    if not on:
        if _git(repo, "rev-parse", "-q", "--verify", f"refs/heads/{branch}", check=False).returncode == 0:
            _git(repo, "checkout", "-q", branch)
        else:
            _git(repo, "checkout", "-q", "-b", branch, "--track", f"{remote}/{branch}")
    _git(repo, "merge", "--ff-only", "-q", target)
    return Outcome(sub.path, "updated", detail, head, target)


def recorded(root: Path, sub: Submodule) -> str | None:
    """The commit the superproject's HEAD records for the submodule."""
    entry = _git(root, "ls-tree", "HEAD", "--", sub.path, check=False).stdout.split()
    return entry[2] if len(entry) >= 3 else None


def bump(root: Path, sub: Submodule) -> str | None:
    """Commit the submodule's pointer, alone, when it moved past what HEAD records; the commit's subject, or None."""
    repo = root / sub.path
    now = _out(repo, "rev-parse", "HEAD")
    was = recorded(root, sub)
    if was == now:
        return None
    count = _count(repo, f"{was}..{now}") if was else 0
    subject = f"Bump {Path(sub.path).name}: {count} commit(s) from {sub.branch or 'its remote'}"
    _git(root, "add", "--", sub.path)
    _git(root, "commit", "-q", "-m", subject, "--", sub.path)
    return subject
