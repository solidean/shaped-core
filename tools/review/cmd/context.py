"""Per-command execution context: the repository under review and the folder the review lives in.

review.py builds one and hands it to each command's `run(args, ctx)`, so a command never reaches back into review.py.

Two repositories, not one.
`home` is where the tool runs and where reviews are kept; `repo` is the checkout being read, which `open()` takes from the review's own config.
They differ whenever the branch under review sits in a worktree, which is the usual shape — and the reason no command but `init` needs to be told a path.
The repository is a parameter rather than an assumption: this tool reviews any git repo, and shaped-core is only the one it ships from.
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from pathlib import Path

import tools.review as review


@dataclass
class Context:
    """Everything a command needs: the repo under review, a git handle on it, and where reviews are kept.

    `repo` starts as `home` and is retargeted by `open()` to whatever the review records, so a command sees one repository and never chooses.
    """

    home: Path
    repo: Path
    git: review.Git
    dir_override: Path | None = None

    @classmethod
    def at(cls, home: Path, *, dir_override: Path | None = None) -> "Context":
        return cls(home=home, repo=home, git=review.Git(home), dir_override=dir_override)

    def die(self, msg: str) -> None:
        print(review.console.red(f"ERROR: {msg}"), file=sys.stderr)
        sys.exit(1)

    def rel(self, p: Path) -> str:
        for base in (self.repo, self.home):
            try:
                return p.relative_to(base).as_posix()
            except ValueError:
                continue
        return str(p)

    def target(self, path: Path) -> None:
        """Point at the checkout to read, dying with an actionable message when it is not one."""
        resolved = path.expanduser().resolve()
        if not resolved.is_dir():
            self.die(f"{resolved} is not a directory")
        try:
            self.repo = review.Git(resolved).toplevel()
        except review.GitError as e:
            self.die(f"{resolved} is not inside a git repository ({e})")
        self.git = review.Git(self.repo)

    def record_repo(self, root: Path) -> str:
        """How `repo` should be written into a review at `root`: relative when both sit on one drive, absolute otherwise."""
        try:
            return Path(os.path.relpath(self.repo, root)).as_posix()
        except ValueError:
            return self.repo.as_posix()

    def paths_for(self, name: str) -> review.ReviewPaths:
        try:
            review.validate_name(name)
        except review.ReviewNameError as e:
            self.die(str(e))
        root = self.dir_override if self.dir_override is not None else review.default_root(self.home, name)
        return review.ReviewPaths(root)

    def open(self, name: str) -> tuple[review.ReviewPaths, review.ReviewConfig]:
        """Load an existing review, dying with an actionable message if it is not there."""
        paths = self.paths_for(name)
        try:
            cfg = review.load(paths.config)
        except review.ConfigError as e:
            self.die(str(e))
        if cfg.repo:
            self.target(paths.root / cfg.repo)
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

    def entries(self, paths: review.ReviewPaths) -> list[review.Entry]:
        """Every entry in navigation order, dying with the file and line on the first malformed one."""
        out = []
        for file in paths.entry_files():
            try:
                out.append(review.parse_entry_file(file))
            except review.ReviewParseError as e:
                self.die(str(e))
        return out

    def answers(self, paths: review.ReviewPaths, entry: review.Entry) -> review.AnswerFile:
        return review.AnswerFile.load(paths.answers_for(entry.path), entry.slug)

    def stamp(self, paths: review.ReviewPaths, cfg: review.ReviewConfig) -> int:
        """Give every unstamped block the current round, and refuse a reworded finalized question.

        Stamping happens whenever the tool touches a review, so a block carries the round it was *written* in
        rather than the round it happened to be read in.
        """
        stamped = 0
        for entry in self.entries(paths):
            answers = self.answers(paths, entry)
            finalized = {name: a.prompt_hash for name, a in answers.answers.items() if not a.tentative}
            try:
                review.check_immutable(entry, finalized)
            except review.ReviewParseError as e:
                self.die(str(e))

            # An ask deleted between rounds must not leave its answer keyed to a question nobody can see.
            moved = answers.reconcile(entry)
            if moved:
                answers.save()
                print(review.console.yellow(
                    f"{entry.slug}: {', '.join(moved)} no longer exist as asks; their answers are kept as orphans"
                ), file=sys.stderr)

            updated = review.stamp_rounds(entry, cfg.next_round)
            if updated is not None:
                review.write_atomic(entry.path, updated)
                stamped += 1
        return stamped

    def check_references(self, paths: review.ReviewPaths, entries: list[review.Entry]) -> list[str]:
        """Change ids an entry names that the ledger does not have.

        A mistyped id is a discharge that silently does not discharge, which makes the coverage report lie —
        the same failure class the block grammar refuses for a mistyped attribute.
        """
        ledger = self.ledger(paths)
        problems = []
        for entry in entries:
            for change_id in sorted(set(entry.referenced_changes())):
                if ledger.resolve(change_id) is None:
                    problems.append(f"{entry.slug}: {change_id} is not in the ledger")
        return problems

    def unaddressed_comments(self, paths: review.ReviewPaths, entries: list[review.Entry]) -> list[tuple[str, object]]:
        """(entry slug, comment) for every sent comment no block claims to answer.

        Computed rather than tracked, the way an undischarged change is: a comment carries no state of its own,
        and `addresses:` on an appended block is the whole record that it was answered.
        A tentative comment is not here — it has not been handed over, so nothing is owed yet.
        """
        out: list[tuple[str, object]] = []
        for entry in entries:
            answers = self.answers(paths, entry)
            claimed = entry.addressed_comments()
            for comment in answers.comments.values():
                if not comment.tentative and comment.id not in claimed:
                    out.append((entry.slug, comment))
        return out

    def discharged(self, entries: list[review.Entry]) -> set[str]:
        """Every change id an ask discharges, across the whole review."""
        out: set[str] = set()
        for entry in entries:
            if entry.state == "open":
                out.update(entry.discharged_changes())
        return out

    def thinly_discharged(self, entries: list[review.Entry]) -> dict[str, list[str]]:
        """Changes accounted for by a meta or orientation entry and by nothing that read them."""
        return review.thinly_discharged(entries)

    def warn_gitignore(self, paths: review.ReviewPaths) -> None:
        """Warn when the review folder would be committed.

        The question is about the repository the folder sits in, which is `home` rather than whatever is under review.
        """
        try:
            ignored = review.Git(self.home).run(["check-ignore", str(paths.root)], timeout=15, check=False)
        except review.GitError:
            return
        if not ignored.strip():
            print(review.console.yellow(
                f"WARNING: {self.rel(paths.root)} is not gitignored in this repo — add `.tmp/` to .gitignore before committing"
            ), file=sys.stderr)
