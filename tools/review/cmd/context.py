"""Per-command execution context: the repository under review and the folder the review lives in.

review.py builds one and hands it to each command's `run(args, ctx)`, so a command never reaches back into review.py.
The repository is a parameter rather than an assumption: this tool reviews any git repo, and shaped-core is only the one it ships from.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

import tools.review as review


@dataclass
class Context:
    """Everything a command needs: the repo, a git handle, and where reviews are kept."""

    repo: Path
    git: review.Git
    dir_override: Path | None = None

    def die(self, msg: str) -> None:
        print(review.console.red(f"ERROR: {msg}"), file=sys.stderr)
        sys.exit(1)

    def rel(self, p: Path) -> str:
        try:
            return p.relative_to(self.repo).as_posix()
        except ValueError:
            return str(p)

    def paths_for(self, name: str) -> review.ReviewPaths:
        try:
            review.validate_name(name)
        except review.ReviewNameError as e:
            self.die(str(e))
        root = self.dir_override if self.dir_override is not None else review.default_root(self.repo, name)
        return review.ReviewPaths(root)

    def open(self, name: str) -> tuple[review.ReviewPaths, review.ReviewConfig]:
        """Load an existing review, dying with an actionable message if it is not there."""
        paths = self.paths_for(name)
        try:
            cfg = review.load(paths.config)
        except review.ConfigError as e:
            self.die(str(e))
        return paths, cfg

    def open_changeset(self, name: str) -> tuple[review.ReviewPaths, review.ReviewConfig]:
        """Load a review that must have a commit range, which a design-only review does not."""
        paths, cfg = self.open(name)
        try:
            cfg.require_changeset()
        except review.ConfigError as e:
            self.die(str(e))
        return paths, cfg

    def ledger(self, paths: review.ReviewPaths) -> review.Ledger:
        return review.Ledger.load(paths.ledger)

    def net_space(self, cfg: review.ReviewConfig) -> review.LineSpace:
        try:
            return review.build_net_space(self.git, cfg.base, cfg.head)
        except review.GitError as e:
            self.die(str(e))

    def warn_gitignore(self, paths: review.ReviewPaths) -> None:
        """Warn when the review folder would be committed.

        A review folder is scratch space, and the first thing a fresh repo does wrong is track it.
        """
        try:
            ignored = self.git.run(["check-ignore", str(paths.root)], timeout=15, check=False)
        except review.GitError:
            return
        if not ignored.strip():
            print(review.console.yellow(
                f"WARNING: {self.rel(paths.root)} is not gitignored in this repo — add `.tmp/` to .gitignore before committing"
            ), file=sys.stderr)
