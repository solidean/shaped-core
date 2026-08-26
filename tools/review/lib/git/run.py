"""The git plumbing every other module goes through, and the flag set it pins.

The flags are not incidental.
A `.gitattributes` `diff=` driver, an external diff tool or a CRLF round-trip would each change the bytes a hunk is made of,
and change ids are a hash of those bytes — so a review would hand out different ids on a different machine.
Everything below runs with text conversion, external diffs and autocrlf disabled, and with rename detection unlimited.
"""

from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path

# Rename detection is capped by default and silently gives up on a large diff, which would make a rename appear as delete+add.
# `-l0` removes the cap, so the answer does not depend on how big the change happens to be.
_DIFF_FLAGS = ["--no-color", "--no-ext-diff", "--no-textconv", "--find-renames", "-l0"]

_CONFIG_FLAGS = ["-c", "core.autocrlf=false", "-c", "core.quotepath=false"]

# The tree of an empty commit, which git guarantees to exist; it stands in for a root commit's missing parent.
EMPTY_TREE = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"


class GitError(Exception):
    """A git invocation that failed, carrying the message the CLI should surface."""


@dataclass(frozen=True)
class Commit:
    sha: str
    short: str
    subject: str
    author: str
    date: str


class Git:
    """A git handle bound to one repository."""

    def __init__(self, repo: Path) -> None:
        self.repo = repo

    def run(self, args: list[str], *, timeout: int = 120, check: bool = True) -> str:
        try:
            proc = subprocess.run(
                ["git", *_CONFIG_FLAGS, *args],
                cwd=str(self.repo), capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=timeout,
            )
        except FileNotFoundError as e:
            raise GitError("git is not on PATH") from e
        except (OSError, subprocess.TimeoutExpired) as e:
            raise GitError(f"`git {' '.join(args[:3])}` could not run: {e}") from e
        if check and proc.returncode != 0:
            detail = (proc.stderr or "").strip().splitlines()
            raise GitError(f"`git {' '.join(args[:4])}` failed: {detail[0] if detail else f'exit {proc.returncode}'}")
        return proc.stdout

    def toplevel(self) -> Path:
        return Path(self.run(["rev-parse", "--show-toplevel"]).strip())

    def remote_url(self, remote: str = "origin") -> str:
        """The remote's fetch URL, or empty when there is no such remote — a repo with no forge is a valid thing to review."""
        return self.run(["remote", "get-url", remote], timeout=15, check=False).strip()

    def rev_parse(self, rev: str) -> str | None:
        """Resolve `rev` to a commit sha, or None if it does not name one."""
        out = self.run(["rev-parse", "--verify", "--quiet", rev + "^{commit}"], timeout=30, check=False)
        return out.strip() or None

    def require_rev(self, rev: str) -> str:
        sha = self.rev_parse(rev)
        if sha is None:
            raise GitError(f"{rev!r} does not name a commit")
        return sha

    def ls_files(self) -> list[str]:
        """Every tracked path, as posix, which is the set an entry can meaningfully refer to.

        Tracked rather than walked on purpose.
        A checkout can hold a whole second copy of itself — `.tmp/worktrees/<name>` is where this tool puts one —
        and a walk would report every basename in the repository as ambiguous.
        """
        out = self.run(["ls-files", "-z"], timeout=60, check=False)
        return [p for p in out.split("\0") if p]

    def merge_base(self, a: str, b: str) -> str | None:
        out = self.run(["merge-base", a, b], timeout=30, check=False)
        return out.strip() or None

    def commits(self, base: str, head: str) -> list[Commit]:
        """The branch's own commits, oldest first, along its first-parent path.

        First-parent is what makes a merged-in branch read as the one commit that brought it,
        which is also the shape the commit-local ingest can map forward.
        """
        fmt = "%H%x1f%h%x1f%s%x1f%an%x1f%ad%x1e"
        out = self.run(["log", "--first-parent", "--reverse", f"--format={fmt}", "--date=short", f"{base}..{head}"])
        commits = []
        for record in out.split("\x1e"):
            record = record.strip("\n")
            if not record:
                continue
            parts = record.split("\x1f")
            if len(parts) == 5:
                commits.append(Commit(*parts))
        return commits

    def which_are_commits(self, candidates: list[str]) -> set[str]:
        """Of these hex strings, the ones that name a commit in this repository.

        One `cat-file --batch-check` rather than a `rev-parse` each, since a page's worth of prose can hold dozens.
        Asking git rather than writing a better regex is also what drops a blob id out of a lockfile diff:
        the object exists, it is simply not a commit.
        """
        if not candidates:
            return set()
        query = "\n".join(f"{c}^{{commit}}" for c in candidates)
        try:
            proc = subprocess.run(
                ["git", *_CONFIG_FLAGS, "cat-file", "--batch-check"],
                cwd=str(self.repo), input=query, capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=30,
            )
        except (OSError, subprocess.TimeoutExpired):
            return set()
        found = set()
        for candidate, line in zip(candidates, proc.stdout.splitlines()):
            if " commit " in line:
                found.add(candidate)
        return found

    def commit_details(self, sha: str) -> dict:
        """Subject, body, author, date and diffstat for one commit — what a popover holds.

        The diffstat is the part missing from the overview today, and it is what answers "how big was that one".
        """
        fmt = "%H%x1f%h%x1f%s%x1f%an%x1f%ad%x1f%b"
        out = self.run(["show", "--no-patch", f"--format={fmt}", "--date=short", sha], check=False)
        parts = out.rstrip("\n").split("\x1f")
        if len(parts) < 6:
            return {}
        stat = self.run(["show", "--shortstat", "--oneline", "--no-color", sha], check=False)
        summary = next((line.strip() for line in stat.splitlines() if " changed" in line), "")
        return {
            "sha": parts[0], "short": parts[1], "subject": parts[2],
            "author": parts[3], "date": parts[4], "body": parts[5].strip(), "stat": summary,
        }

    def has_merges(self, base: str, head: str) -> list[str]:
        """Shas of merge commits on the first-parent path, which commit-local mapping cannot follow."""
        out = self.run(["rev-list", "--merges", "--first-parent", f"{base}..{head}"])
        return [line.strip() for line in out.splitlines() if line.strip()]

    def diff(self, base: str, head: str, *, context: int, paths: list[str] | None = None) -> str:
        """A two-dot diff between two commits, with the pinned flag set."""
        args = ["diff", *_DIFF_FLAGS, f"--unified={context}", base, head]
        if paths:
            args += ["--", *paths]
        return self.run(args)

    def numstat(self, base: str, head: str) -> list[tuple[str, str, str]]:
        """(added, removed, path) per changed file; a binary file reports '-' for both counts."""
        out = self.run(["diff", *_DIFF_FLAGS, "--numstat", "-z", base, head])
        fields = [f for f in out.split("\0") if f]
        rows: list[tuple[str, str, str]] = []
        i = 0
        while i < len(fields):
            head_field = fields[i]
            parts = head_field.split("\t")
            if len(parts) < 3:
                i += 1
                continue
            added, removed, name = parts[0], parts[1], parts[2]
            if name == "":
                # A rename reports an empty name followed by the old and new paths as separate NUL-terminated fields.
                old, new = fields[i + 1], fields[i + 2]
                rows.append((added, removed, new if new else old))
                i += 3
                continue
            rows.append((added, removed, name))
            i += 1
        return rows

    def pr_body(self, head_spec: str) -> str:
        """The PR body for a branch, when `gh` is installed and the branch has one.

        Best-effort by design: a review must init on a repo with no forge, no `gh`, and no network.
        """
        try:
            proc = subprocess.run(
                ["gh", "pr", "view", head_spec, "--json", "title,body,author,url"],
                cwd=str(self.repo), capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=5,
            )
        except (OSError, subprocess.TimeoutExpired):
            return ""
        return proc.stdout if proc.returncode == 0 else ""
