"""Which files and lines a lint or format run is restricted to.

Two change sets answer the same question from different ends.
The **working tree** — git-dirty and untracked files — is what the next commit will carry, and it is what the pre-commit gates want.
A **revision** — a single commit or an `A..B` range — is that commit's own diff, and it is what re-checking already-committed work wants.
Once work is committed the working tree is clean, so a dirty-only re-check inspects nothing and reports green; a revision scope is the answer to that.

A single revision means its first-parent diff, so `--commit <a merge>` yields everything the merge brought in rather than only the conflict resolutions.

A revision scope fails loudly where the working-tree scope fails silently.
An empty working tree is the ordinary "nothing to commit".
An empty revision usually means a typo'd hash, and returning nothing there would pass every gate green without looking at a file.
"""

from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from ..core import console, profile


class ChangeScopeError(Exception):
    """An unusable revision spec, carrying the message the CLI should surface."""


@dataclass(frozen=True)
class ChangeScope:
    """Which change set a lint or format run is restricted to.

    `revision` None means the working tree — dirty and untracked files.
    Otherwise it is a git revision or an `A..B` range, and the change set is that diff instead.
    A scope of `None` — no ChangeScope at all — means the whole tree, which is a third state and deliberately not expressible here.
    """

    revision: str | None = None

    @property
    def is_working_tree(self) -> bool:
        return self.revision is None

    def phrase(self, noun: str) -> str:
        """`noun` narrowed to this scope, as a summary line would say it.

        The two scopes want the qualifier on opposite sides — "dirty libs/ sources" but "libs/ sources in abc123" — so the caller hands over the noun rather than a prefix.
        """
        return f"dirty {noun}" if self.revision is None else f"{noun} in {self.revision}"

    def rerun_flag(self) -> str:
        """The flag that reproduces this scope, for a hint in a failure message."""
        return " --dirty-only" if self.revision is None else f" --commit {self.revision}"


# The tree of an empty commit, which git guarantees to exist.
# A root commit has no parent to diff against, and this stands in for one.
_EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"

# A whole untracked file is "changed", and its length is not worth a stat — the linter clamps anyway.
_ALL_LINES = 0xFFFFFFFF

# At most this many drifted files are named before the warning collapses to a count.
_DRIFT_NAMES = 5

# Heads already warned about, so a `check` run reporting one drift per gate says it once instead.
_warned_heads: set[str] = set()


def _git(root: Path, args: list[str], *, timeout: int = 60) -> subprocess.CompletedProcess | None:
    """Run a git command under `root`; return None if it could not run at all."""
    try:
        with profile.span(" ".join(["git", *args[:2]]), type="git"):
            return subprocess.run(
                ["git", *args],
                cwd=str(root), capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout,
            )
    except (OSError, subprocess.TimeoutExpired):
        return None


def _git_or_raise(root: Path, args: list[str], *, timeout: int = 60) -> str:
    """Run a git command and return its stdout, raising ChangeScopeError on any failure.

    The loud counterpart to `_git`, for the revision path where an empty result would silently scope a gate to nothing.
    """
    out = _git(root, args, timeout=timeout)
    if out is None:
        raise ChangeScopeError(f"could not run `git {' '.join(args)}` (git missing, or it timed out)")
    if out.returncode != 0:
        detail = out.stderr.strip().splitlines()
        reason = detail[0] if detail else f"exit {out.returncode}"
        raise ChangeScopeError(f"`git {' '.join(args)}` failed: {reason}")
    return out.stdout


def _rev_parse(root: Path, rev: str) -> str | None:
    """Resolve `rev` to a commit hash, or None if it does not name one."""
    out = _git(root, ["rev-parse", "--verify", "--quiet", rev + "^{commit}"], timeout=30)
    if out is None or out.returncode != 0:
        return None
    return out.stdout.strip() or None


def resolve_range(root: Path, revision: str) -> tuple[str, str]:
    """Expand a revision spec into the (base, head) pair the diffs run against.

    A spec containing `..` is already a range, and both ends are used as given, which covers `A..B` and `A...B` alike.
    Anything else is a single commit, expanded to its first-parent diff — the reason `--commit <a merge>` yields the whole merged-in change set.
    An omitted end defaults to HEAD, matching git.
    Raises ChangeScopeError if either end does not name a commit.
    """
    if ".." in revision:
        base_spec, _, head_spec = revision.partition("..")
        head_spec = head_spec.lstrip(".")  # the third dot of `A...B`
        base_spec = base_spec.strip() or "HEAD"
        head_spec = head_spec.strip() or "HEAD"
        for spec in (base_spec, head_spec):
            if _rev_parse(root, spec) is None:
                raise ChangeScopeError(f"{spec!r} does not name a commit (from --commit {revision})")
        return base_spec, head_spec

    if _rev_parse(root, revision) is None:
        raise ChangeScopeError(f"{revision!r} does not name a commit")
    parent = _rev_parse(root, revision + "^")
    return (parent or _EMPTY_TREE), revision


def _split_z(stdout: str) -> list[str]:
    """Split a `-z` git listing, whose entries are NUL-terminated and therefore never quoted."""
    return [entry for entry in stdout.split("\0") if entry]


def _dirty_files(root: Path) -> list[Path]:
    """Files that are git-dirty or untracked — what is reasonably part of the next commit.

    Deletions are dropped, since there is nothing to format, and a rename yields its new path.
    Returns absolute paths, with nonexistent entries filtered out.
    """
    out = _git(root, ["status", "--porcelain", "--untracked-files=all"], timeout=30)
    if out is None or out.returncode != 0:
        return []

    paths: list[Path] = []
    for line in out.stdout.splitlines():
        if len(line) < 4:
            continue
        # Porcelain v1: a 2-char status field, a space, then the path; a rename is 'R  <old> -> <new>'.
        # A 'D' in either status column is a deletion.
        status, rest = line[:2], line[3:]
        if "D" in status:
            continue
        path_part = rest.split(" -> ")[-1].strip().strip('"')
        p = (root / path_part).resolve()
        if p.is_file():
            paths.append(p)
    return paths


def _revision_files(root: Path, base: str, head: str) -> list[Path]:
    """The files a revision's diff touched, as absolute paths.

    Deletions are excluded and a rename yields its new path, matching the working-tree set.
    A path that no longer exists on disk is dropped, since nothing can be linted through it.
    """
    stdout = _git_or_raise(root, ["diff", "--name-only", "--diff-filter=d", "-z", base, head])
    paths = [(root / name).resolve() for name in _split_z(stdout)]
    return [p for p in paths if p.is_file()]


def _warn_on_drift(root: Path, head: str, selected: set[Path]) -> None:
    """Warn about files whose working-tree content differs from the scope's head commit.

    Only the line ranges are affected, which is why this sits on that path and not on file discovery.
    They are numbered against the commit, but the linter reads the file from disk, so a file that moved on since then has its findings filtered against stale lines.
    A warning rather than an error: re-checking a commit you just made is the common case and is drift-free, and the rest are still worth running.
    """
    if head in _warned_heads:
        return
    out = _git(root, ["diff", "--name-only", "-z", head])
    if out is None or out.returncode != 0:
        return
    drifted = sorted({(root / name).resolve() for name in _split_z(out.stdout)} & selected)
    if not drifted:
        return
    _warned_heads.add(head)

    shown = ", ".join(str(p.relative_to(root)) for p in drifted[:_DRIFT_NAMES])
    more = f" (+{len(drifted) - _DRIFT_NAMES} more)" if len(drifted) > _DRIFT_NAMES else ""
    print(
        console.yellow(
            f"WARNING: {len(drifted)} file(s) changed since {head}, "
            f"so their line ranges may not match what is on disk: {shown}{more}"
        ),
        file=sys.stderr,
    )


def changed_files(root: Path, scope: ChangeScope) -> list[Path]:
    """The absolute paths the scope covers, before any linter's own roots and suffixes narrow them."""
    if scope.is_working_tree:
        return _dirty_files(root)

    base, head = resolve_range(root, scope.revision)
    return _revision_files(root, base, head)


# A hunk header names the post-image side as `+<start>[,<count>]`, and `-U0` makes every hunk a bare change.
_HUNK_RE = re.compile(r"@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def _parse_diff_ranges(root: Path, stdout: str, ranges: dict[Path, list[tuple[int, int]]]) -> None:
    """Accumulate the post-image line ranges of a `-U0` unified diff into `ranges`.

    A pure-deletion hunk marks the surviving line above it, which is the one the edit could have broken.
    """
    current: Path | None = None
    for line in stdout.splitlines():
        if line.startswith("+++ "):
            target = line[4:].strip()
            if target == "/dev/null":
                current = None
            else:
                current = (root / (target[2:] if target.startswith("b/") else target)).resolve()
            continue
        if current is None or not line.startswith("@@"):
            continue

        m = _HUNK_RE.match(line)
        if not m:
            continue
        start = int(m.group(1))
        count = int(m.group(2)) if m.group(2) is not None else 1
        if count == 0:
            ranges.setdefault(current, []).append((max(1, start), max(1, start)))
        else:
            ranges.setdefault(current, []).append((start, start + count - 1))


def changed_line_ranges(root: Path, scope: ChangeScope) -> dict[Path, list[tuple[int, int]]]:
    """The 1-based line ranges each changed file changed, as absolute paths -> [(first, last), ...].

    This is what makes a scoped prose run line-exact instead of file-wide.
    A prose finding sits on one line, so it either changed or it did not; a code finding can be caused by
    a line the edit never touched, which is why only prose rules are scoped this way.

    For the working tree, tracked changes come from `git diff -U0 HEAD`, so staged and unstaged both count,
    and an untracked file has no diff and is reported as changed end to end.
    For a revision the ranges are numbered against its head commit, which is why `changed_files` warns when the tree has moved on.
    """
    ranges: dict[Path, list[tuple[int, int]]] = {}

    if not scope.is_working_tree:
        base, head = resolve_range(root, scope.revision)
        _parse_diff_ranges(root, _git_or_raise(root, ["diff", "--unified=0", "--no-color", base, head]), ranges)
        _warn_on_drift(root, head, set(ranges))
        return ranges

    out = _git(root, ["diff", "--unified=0", "--no-color", "HEAD"])
    if out is not None and out.returncode == 0:
        _parse_diff_ranges(root, out.stdout, ranges)

    untracked = _git(root, ["ls-files", "--others", "--exclude-standard"], timeout=30)
    if untracked is not None and untracked.returncode == 0:
        for name in untracked.stdout.splitlines():
            if not name.strip():
                continue
            p = (root / name.strip()).resolve()
            if p.is_file():
                ranges.setdefault(p, []).append((1, _ALL_LINES))

    return ranges


def format_changed_line_spec(ranges: dict[Path, list[tuple[int, int]]]) -> str:
    """Render `changed_line_ranges` as shaped-linter's `--changed-lines` spec: `<path>:a-b,c-d` per line."""
    lines = []
    for path in sorted(ranges):
        spec = ",".join(f"{first}-{last}" for first, last in ranges[path])
        lines.append(f"{path}:{spec}")
    return "\n".join(lines) + ("\n" if lines else "")
