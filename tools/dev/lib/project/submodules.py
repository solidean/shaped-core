"""Bring each submodule up to the commit the superproject records for it, or with `tip` to its tracked branch, never at the cost of local work.

By default a submodule goes no further than the recorded pointer, so the working tree matches what the superproject's HEAD records.
A submodule ahead of its pointer is left there: that is local work, or a move to the tip someone asked for.
With `tip` it goes to the head of the branch `.gitmodules` names, which runs ahead of the pointer until a bump commits it.

Either way it moves only by a fast-forward, and only from one of two states:
- the submodule is on its tracked branch, with nothing uncommitted;
- its HEAD is detached, which is how `git submodule update` leaves it.
The fast-forward leaves the tracked branch checked out wherever the branch can reach the target, so a commit made there is never stranded.
Everything else is reported and left alone: uncommitted changes, another branch checked out, a history that diverged from the target, and for `tip` unpushed commits.
So nothing is ever reset, rebased, merged or rewritten, and no commit can be stranded.
A submodule `.gitmodules` declares but that is not checked out yet, say one a pull just added, is initialized first at the recorded commit, which leaves it detached within its branch.
So is a missing submodule of a submodule, at any depth; one already checked out there is left where it is.
"""

from __future__ import annotations

import dataclasses
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
    # "initialized", "updated", "up-to-date", "ahead", "skipped" or "refused"; dry runs say "would init" or "would update".
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


def init_nested(root: Path, sub: Submodule, *, dry_run: bool = False) -> list[Outcome]:
    """Initialize the submodules `sub` declares but has not checked out, at any depth; paths in the outcomes are relative to `root`."""
    repo = root / sub.path
    outcomes = [dataclasses.replace(init(repo, n, dry_run=dry_run), path=f"{sub.path}/{n.path}")
                for n in list_submodules(repo, missing=True)]
    for n in list_submodules(repo):
        outcomes += init_nested(root, dataclasses.replace(n, path=f"{sub.path}/{n.path}"), dry_run=dry_run)
    return outcomes


def sync(root: Path, sub: Submodule, *, dry_run: bool = False, fetch: bool = True, tip: bool = False) -> Outcome:
    """Fast-forward one submodule to its recorded commit, or with `tip` to its tracked branch, or say why not."""
    if tip:
        if sub.branch is None:
            return Outcome(sub.path, "skipped", "no `branch =` in .gitmodules")
        return sync_branch(root / sub.path, sub.path, sub.branch, dry_run=dry_run, fetch=fetch)
    try:
        return _sync_recorded(root, sub, dry_run=dry_run, fetch=fetch)
    except _GitError as e:
        return Outcome(sub.path, "refused", str(e))


def sync_branch(repo: Path, path: str, branch: str, *, dry_run: bool = False, fetch: bool = True) -> Outcome:
    """Fast-forward the clone at `repo` to the head of `branch` under the same rules as a `tip` sync; `path` labels the outcome.

    Not bound to a submodule: a clone the superproject records nothing for, a companion repository, moves this way too.
    """
    try:
        return _sync_tip(repo, path, branch, dry_run=dry_run, fetch=fetch)
    except _GitError as e:
        return Outcome(path, "refused", str(e))


def _is_ancestor(repo: Path, a: str, b: str) -> bool:
    return _git(repo, "merge-base", "--is-ancestor", a, b, check=False).returncode == 0


def _has_commit(repo: Path, rev: str) -> bool:
    return _git(repo, "cat-file", "-e", f"{rev}^{{commit}}", check=False).returncode == 0


def _sync_recorded(root: Path, sub: Submodule, *, dry_run: bool, fetch: bool) -> Outcome:
    repo = root / sub.path
    target = recorded(root, sub)
    if target is None:
        return Outcome(sub.path, "skipped", "the superproject records no commit for it yet")
    remote = (_git(repo, "config", f"branch.{sub.branch}.remote", check=False).stdout.strip() if sub.branch else "") or "origin"
    if not _has_commit(repo, target) and fetch:
        _git(repo, "fetch", "-q", remote)
    if not _has_commit(repo, target):
        return Outcome(sub.path, "refused", f"the recorded {target[:10]} is not on {remote}; was the submodule pushed?")

    head = _out(repo, "rev-parse", "HEAD")
    on = _git(repo, "symbolic-ref", "-q", "--short", "HEAD", check=False).stdout.strip()
    where = on or "detached"
    if head == target:
        return Outcome(sub.path, "up-to-date", f"{where} at the recorded {head[:10]}", head, head)
    if _is_ancestor(repo, target, head):
        return Outcome(sub.path, "ahead", f"{where}, {_count(repo, f'{target}..{head}')} commit(s) past the recorded {target[:10]}; a bump would commit them",
                       head, head)
    if not _is_ancestor(repo, head, target):
        return Outcome(sub.path, "refused", f"{where} at {head[:10]}, which diverged from the recorded {target[:10]}")
    if on and on != sub.branch:
        return Outcome(sub.path, "skipped", f"{on} is checked out, and the recorded {target[:10]} is ahead of it")
    # Only a move needs a clean tree; a submodule at or past its pointer is reported however dirty.
    if _out(repo, "status", "--porcelain", "--untracked-files=no"):
        return Outcome(sub.path, "refused", "uncommitted changes, and the recorded commit is ahead")
    if _git(repo, "rev-parse", "-q", "--verify", "MERGE_HEAD", check=False).returncode == 0:
        return Outcome(sub.path, "refused", "a merge is in progress")

    behind = _count(repo, f"HEAD..{target}")
    # A detached HEAD goes back onto the tracked branch when that branch can fast-forward to the target too.
    branch = on or None
    if not on and sub.branch:
        local = _git(repo, "rev-parse", "-q", "--verify", f"refs/heads/{sub.branch}", check=False).stdout.strip()
        tracking = f"refs/remotes/{remote}/{sub.branch}"
        if local and _is_ancestor(repo, local, target):
            branch = sub.branch
        elif not local and _has_commit(repo, tracking) and _is_ancestor(repo, target, tracking):
            branch = sub.branch
    detail = f"{where}: {head[:10]}..{target[:10]}, {behind} commit(s) to the recorded pointer"
    if branch and not on:
        detail += f", on {branch}"
    if dry_run:
        return Outcome(sub.path, "would update", detail, head, target)

    if on:
        _git(repo, "merge", "--ff-only", "-q", target)
    elif branch and _git(repo, "rev-parse", "-q", "--verify", f"refs/heads/{branch}", check=False).returncode == 0:
        _git(repo, "checkout", "-q", branch)
        _git(repo, "merge", "--ff-only", "-q", target)
    elif branch:
        _git(repo, "checkout", "-q", "-b", branch, target)
        _git(repo, "branch", "-q", f"--set-upstream-to={remote}/{branch}", branch)
    else:
        _git(repo, "checkout", "-q", "--detach", target)
    return Outcome(sub.path, "updated", detail, head, target)


def _sync_tip(repo: Path, path: str, branch: str, *, dry_run: bool, fetch: bool) -> Outcome:
    remote = _git(repo, "config", f"branch.{branch}.remote", check=False).stdout.strip() or "origin"
    tracking = f"refs/remotes/{remote}/{branch}"

    # Untracked files are not judged: a fast-forward never touches one, and git refuses rather than overwrite it.
    if _out(repo, "status", "--porcelain", "--untracked-files=no"):
        return Outcome(path, "refused", "uncommitted changes")
    if _git(repo, "rev-parse", "-q", "--verify", "MERGE_HEAD", check=False).returncode == 0:
        return Outcome(path, "refused", "a merge is in progress")
    if fetch:
        _git(repo, "fetch", "-q", remote, f"+refs/heads/{branch}:{tracking}")
    target = _out(repo, "rev-parse", "--verify", f"{tracking}^{{commit}}")
    head = _out(repo, "rev-parse", "HEAD")
    on = _git(repo, "symbolic-ref", "-q", "--short", "HEAD", check=False).stdout.strip()

    if on and on != branch:
        return Outcome(path, "skipped", f"{on} is checked out, and it tracks {branch}")
    if on:
        ahead = _count(repo, f"{target}..HEAD")
        if ahead:
            return Outcome(path, "refused", f"{ahead} unpushed commit(s) on {branch}")
    else:
        if _git(repo, "merge-base", "--is-ancestor", "HEAD", target, check=False).returncode != 0:
            return Outcome(path, "refused", f"detached at {head[:10]}, which {remote}/{branch} does not contain")
        local = _git(repo, "rev-parse", "-q", "--verify", f"refs/heads/{branch}", check=False).stdout.strip()
        if local and _count(repo, f"{target}..{local}"):
            return Outcome(path, "refused", f"local {branch} has {_count(repo, f'{target}..{local}')} unpushed commit(s)")

    behind = _count(repo, f"HEAD..{target}")
    if on and not behind:
        return Outcome(path, "up-to-date", f"{branch} at {head[:10]}", head, head)
    if on:
        detail = f"{branch}: {head[:10]}..{target[:10]}, {behind} commit(s)"
    else:
        detail = f"checks {branch} out at the detached {head[:10]}" + (f", then {behind} commit(s) to {target[:10]}" if behind else "")
    if dry_run:
        return Outcome(path, "would update", detail, head, target)

    if not on:
        if _git(repo, "rev-parse", "-q", "--verify", f"refs/heads/{branch}", check=False).returncode == 0:
            _git(repo, "checkout", "-q", branch)
        else:
            _git(repo, "checkout", "-q", "-b", branch, "--track", f"{remote}/{branch}")
    _git(repo, "merge", "--ff-only", "-q", target)
    return Outcome(path, "updated", detail, head, target)


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
