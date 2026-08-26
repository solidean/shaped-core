"""The index a file reference resolves against.

Three lookups, in order: the exact repo-relative path, a unique path suffix, and a unique basename.
`lib/render/markdown.py` is how someone working inside `tools/review/` writes it, and `markdown.py` is how anyone
refers to a file in a sentence — neither resolved before this existed, so most references simply missed.

Built from `git ls-files` rather than from a filesystem walk.
That is not an optimization: a checkout can hold a complete second copy of itself, and a walk would then report
almost every basename as ambiguous.

The review folder is indexed beside it.
`review.toml` and the entry files under `entries/` are named as readily as source is, and they are not in
the repository under review — which is why a path carries the root it was found under rather than only its name.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from ..git.run import Git

# The states a reference can be in, which is what decides both its decoration and whether `validate` fails on it.
RESOLVED = "resolved"
AMBIGUOUS = "ambiguous"
MISSING = "missing"

# What a review folder contributes, relative to its own root.
_REVIEW_GLOBS = ("review.toml", "entries/*.md", "answers/*.json", "rounds/*.md", "attachments/*")


@dataclass(frozen=True)
class Resolution:
    """What one reference resolved to."""

    state: str
    path: str = ""
    candidates: tuple[str, ...] = ()

    @property
    def ok(self) -> bool:
        return self.state == RESOLVED


class RepoIndex:
    """Every path an entry can meaningfully refer to, indexed for the three lookups."""

    def __init__(self, roots: dict[str, Path] | None = None) -> None:
        self._root_of: dict[str, Path] = dict(roots or {})
        self.paths = sorted(self._root_of)
        self._exact = set(self.paths)
        self._by_suffix: dict[str, list[str]] = {}
        # Every suffix a tracked path actually ends in, which is what tells a reference from a dotted identifier.
        # `git.has_merges` is prose about code; `vector.hh` is a file.
        # Only the repository can say which is which.
        self.suffixes: set[str] = set()
        for path in self.paths:
            parts = path.split("/")
            for start in range(len(parts)):
                self._by_suffix.setdefault("/".join(parts[start:]), []).append(path)
            _, dot, suffix = parts[-1].rpartition(".")
            if dot and suffix:
                self.suffixes.add(suffix.lower())

    @staticmethod
    def build(repo: Path, review_root: Path | None = None) -> RepoIndex:
        roots: dict[str, Path] = {}
        try:
            for path in Git(repo).ls_files():
                roots[path] = repo
        except Exception:  # noqa: BLE001 — a repo git cannot read is an empty index, never a failed render.
            pass
        if review_root is not None and review_root.is_dir():
            for pattern in _REVIEW_GLOBS:
                for found in sorted(review_root.glob(pattern)):
                    if found.is_file():
                        roots.setdefault(found.relative_to(review_root).as_posix(), review_root)
        return RepoIndex(roots)

    def looks_like_a_path(self, ref: str) -> bool:
        """Whether this is a reference at all, rather than prose that happens to hold a dot.

        A slash settles it.
        Otherwise the suffix has to be one some tracked file really uses, which keeps `sr::window.headless`
        and `git.has_merges` out without a hand-maintained list of extensions.
        """
        # A leading slash or a scheme means a URL or a route — `/favicon.ico`, `vscode://file/x` — never a path
        # relative to a repository root.
        if ref.startswith("/") or "://" in ref:
            return False
        if "/" in ref:
            return True
        _, dot, suffix = ref.rpartition(".")
        return bool(dot) and suffix.lower() in self.suffixes

    def resolve(self, ref: str) -> Resolution:
        """Resolve one reference, saying which of the three ways it went — or why it did not."""
        ref = ref.strip().lstrip("./")
        if not ref:
            return Resolution(MISSING)
        if ref in self._exact:
            return Resolution(RESOLVED, ref)
        candidates = self._by_suffix.get(ref, [])
        if len(candidates) == 1:
            return Resolution(RESOLVED, candidates[0])
        if candidates:
            return Resolution(AMBIGUOUS, candidates=tuple(candidates))
        return Resolution(MISSING)

    def absolute(self, path: str) -> Path | None:
        """Where a resolved path actually is, which is not always under the repository."""
        root = self._root_of.get(path)
        return None if root is None else root / path

    def __len__(self) -> int:
        return len(self.paths)
