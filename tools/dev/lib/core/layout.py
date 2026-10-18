"""Where a project keeps its files: the one place lib/ learns a repo's shape.

Two kinds of path meet in the driver, and they are kept apart on purpose.

A `Layout` describes the PROJECT's first-party tree — what clang-format owns, what shaped-linter and the crossref check walk, how coverage groups libraries.
It is policy: dev.py declares one in its `Policy`, and every lib/ function that walks the tree takes it explicitly, so a consumer cannot silently inherit ours.

`DRIVER_ROOT` is where the DRIVER's own files live — the pinned clang-format and its fetcher, the emsdk pin, the vendored TLS stack doctor reports on.
Those belong to shaped-core whichever project runs the driver, so they are found relative to this file rather than to the project root.
In shaped-core itself the two roots coincide; in a project consuming shaped-core as a submodule, DRIVER_ROOT is the submodule.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

# tools/dev/lib/core/layout.py -> the repository the driver is part of.
DRIVER_ROOT = Path(__file__).resolve().parents[4]

# What an extra root can opt into.
# shaped-linter selects no rule family per run, so `prose` and `code` split by file kind instead:
# markdown carries prose rules alone, while C++ and Python get every rule, the prose rules over their comments included.
EXTRA_ROOT_CHECKS = ("crossrefs", "prose", "code")


@dataclass(frozen=True)
class ExtraRoot:
    """A directory outside the first-party roots that some checks still own — typically a submodule we write docs in.

    `include` and `exclude` are gitignore-style globs relative to `path`: an empty `include` takes every file, and `exclude` wins.
    A glob without a `/` matches a name at any depth, and one naming a directory takes everything below it.
    """

    # Relative to the project root.
    path: str

    # A submodule is listed and diffed through its own git, since the superproject sees only its gitlink.
    submodule: bool = False

    # A subset of EXTRA_ROOT_CHECKS.
    checks: tuple[str, ...] = EXTRA_ROOT_CHECKS

    include: tuple[str, ...] = ()
    exclude: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        unknown = [c for c in self.checks if c not in EXTRA_ROOT_CHECKS]
        if unknown:
            raise ValueError(f"extra root {self.path!r}: unknown check(s) {unknown}; expected some of {EXTRA_ROOT_CHECKS}")
        if self.path.startswith("/") or self.path.endswith("/") or not self.path:
            raise ValueError(f"extra root {self.path!r}: the path must be relative, without a trailing slash")


@dataclass(frozen=True)
class Layout:
    """A project's first-party tree, as directories relative to its root."""

    # The directories whose `.cc`/`.hh` files clang-format owns.
    # A whitelist: vendored code must not be reformatted, and a stray source elsewhere should not silently become ours.
    source_roots: tuple[str, ...]

    # The directories shaped-linter owns, code and prose; wider than `source_roots`, since prose lives in docs too.
    lint_roots: tuple[str, ...]

    # First-party files at the repo root, which is not a directory to walk wholesale.
    lint_root_files: tuple[str, ...]

    # Directories the crossref check scans for references.
    crossref_scan_dirs: tuple[str, ...]

    # First path segments that mark a reference as possibly repo-root-relative (`docs/...`), each with its trailing slash.
    crossref_root_segments: tuple[str, ...]

    # The directory libraries live under, as `<library_dir>/<category>/<lib>/`; coverage reports per library on that key.
    library_dir: str

    # Directories some checks own beyond the roots above, each with its own checks and globs.
    # A link inside one resolves against the extra root rather than the project, which is how its own readme reads it.
    extra_roots: tuple[ExtraRoot, ...] = ()

    def extra_roots_with(self, check: str) -> tuple[ExtraRoot, ...]:
        """The extra roots that opted into `check`."""
        return tuple(r for r in self.extra_roots if check in r.checks)


SHAPED_CORE_LAYOUT = Layout(
    source_roots=("libs", "tools/instruction-tracer", "tools/shaped-linter"),
    lint_roots=("libs", "tools", "docs", ".claude/skills"),
    lint_root_files=("CLAUDE.md", "README.md", "dev.py"),
    crossref_scan_dirs=("libs", "docs", "tools", ".claude"),
    crossref_root_segments=("docs/", "libs/", "tools/", ".claude/"),
    library_dir="libs",
)
