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


SHAPED_CORE_LAYOUT = Layout(
    source_roots=("libs", "tools/instruction-tracer", "tools/shaped-linter"),
    lint_roots=("libs", "tools", "docs", ".claude/skills"),
    lint_root_files=("CLAUDE.md", "README.md", "dev.py"),
    crossref_scan_dirs=("libs", "docs", "tools", ".claude"),
    crossref_root_segments=("docs/", "libs/", "tools/", ".claude/"),
    library_dir="libs",
)
