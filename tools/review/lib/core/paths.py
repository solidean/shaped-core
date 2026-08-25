"""Every path inside a review folder, in one place.

Nothing else in the tool builds a path by joining strings, so the on-disk layout is changed here and nowhere else.
tools/review/docs/review-folder.md is the layout this encodes.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

# A review name becomes a directory name and appears in URLs, so it stays boring on purpose.
_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")

# The entry file's number prefix is what orders the navigation, and gaps are what let a later round insert between two entries.
_ENTRY_RE = re.compile(r"^(\d{3})-([a-z0-9][a-z0-9-]*)\.md$")


class ReviewNameError(Exception):
    """An unusable review name, carrying the message the CLI should surface."""


def validate_name(name: str) -> str:
    """Return `name` if it is a usable review name, else raise ReviewNameError."""
    if not _NAME_RE.match(name):
        raise ReviewNameError(
            f"{name!r} is not a usable review name: lowercase letters, digits, dot, dash and underscore, at most 64 chars"
        )
    return name


@dataclass(frozen=True)
class ReviewPaths:
    """The review folder's layout, rooted at `root`."""

    root: Path

    @property
    def config(self) -> Path:
        return self.root / "review.toml"

    @property
    def changes_dir(self) -> Path:
        return self.root / "changes"

    @property
    def ledger(self) -> Path:
        return self.changes_dir / "ledger.jsonl"

    @property
    def entries_dir(self) -> Path:
        return self.root / "entries"

    @property
    def answers_dir(self) -> Path:
        return self.root / "answers"

    @property
    def rounds_dir(self) -> Path:
        return self.root / "rounds"

    @property
    def log(self) -> Path:
        return self.root / "log.jsonl"

    @property
    def signal(self) -> Path:
        """The server's one-shot mailbox that `round --wait` polls for."""
        return self.root / ".signal"

    def change_diff(self, change_id: str) -> Path:
        return self.changes_dir / f"{change_id}.diff"

    def round_file(self, number: int) -> Path:
        return self.rounds_dir / f"round-{number}.md"

    def entry_files(self) -> list[Path]:
        """Entry files in navigation order, which is their numeric prefix order."""
        if not self.entries_dir.is_dir():
            return []
        return sorted((p for p in self.entries_dir.iterdir() if _ENTRY_RE.match(p.name)), key=lambda p: p.name)

    def answers_for(self, entry_file: Path) -> Path:
        return self.answers_dir / (entry_file.stem + ".json")

    def create(self) -> None:
        for d in (self.root, self.changes_dir, self.entries_dir, self.answers_dir, self.rounds_dir):
            d.mkdir(parents=True, exist_ok=True)

    def exists(self) -> bool:
        return self.config.is_file()


def entry_slug(entry_file: Path) -> str:
    """The stable id of an entry: its filename without the extension, e.g. `040-correctness-index`."""
    return entry_file.stem


def reviews_root(repo: Path) -> Path:
    """The directory a repository's reviews live in, which is scratch space rather than tracked content."""
    return repo / ".tmp" / "reviews"


def default_root(repo: Path, name: str) -> Path:
    """Where a review lives unless `--dir` says otherwise."""
    return reviews_root(repo) / name
