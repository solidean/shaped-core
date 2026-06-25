#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Vendor mimalloc in-tree at a pinned commit.

shaped-core builds offline and reproducibly, so third-party dependencies live
in-tree rather than as submodules or fetched-at-configure-time packages. mimalloc
is shaped-core's first such vendored dependency (see the SC_USE_VENDORED_MIMALLOC
option in the root CMakeLists.txt). This script regenerates the vendored copy from
a pinned upstream commit: it shallow-clones the tag into a transient `.clone/` dir,
asserts the tag resolves to the pinned hash (the hash is the authority; the tag is
a human-readable convenience), copies the minimal subset we actually build
(LICENSE + include/ + src/), and deletes the clone.

Only `src/static.c` is compiled by our CMake (a single translation unit that
`#include`s the other `.c` files), but the whole `src/` tree must be on disk for
those includes to resolve — see the CMakeLists.txt next to this script.

Re-running is idempotent: the previously vendored files are wiped first, so files
dropped upstream do not linger. The vendored payload (include/, src/, LICENSE) is
committed to the repo; this script is only needed to bump or re-vet the version.
"""

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# Pinned upstream. The tag is for humans; PIN_HASH is the authority — the clone is
# rejected unless the tag resolves to exactly this commit. Bump PIN_TAG and PIN_HASH
# together, only after vetting the new commit.
REPO_URL = "https://github.com/microsoft/mimalloc"
PIN_TAG = "v3.3.2"
PIN_HASH = "30b2d9d89099bee08e9f67a1ffb3e12e7ba45227"

# This script lives in extern/mimalloc/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# What we keep from the upstream tree. Everything else (test/, cmake/, bin/, doc/,
# ide/, .github/, packaging, ...) is intentionally dropped — we build static.c with
# our own CMake and never use mimalloc's build/install machinery.
COPY_FILES = ["LICENSE"]
COPY_DIRS = ["include", "src"]

# Within the copied dirs, drop non-source cruft that sweeps in (readme/docs, the
# Windows ETW manifest + tracing profile). We compile only C/C++ sources.
EXCLUDE_SUFFIXES = {".md", ".man", ".wprp"}


def _ignore_cruft(_dir: str, names: list[str]) -> set[str]:
    return {n for n in names if Path(n).suffix.lower() in EXCLUDE_SUFFIXES}


def _force_rmtree(path: Path) -> None:
    """rmtree that survives Windows: git packs the .git objects read-only, which
    blocks os.unlink — clear the read-only bit on error and retry."""

    def on_error(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)

    shutil.rmtree(path, onexc=on_error)


def run(*args: str, cwd: Path | None = None) -> str:
    """Run a git command, returning stripped stdout; abort loudly on failure."""
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"command failed: {' '.join(args)}\n{result.stderr.strip()}")
    return result.stdout.strip()


def main() -> int:
    # Clean slate: a stale clone or partial previous run must not leak in.
    if CLONE.exists():
        _force_rmtree(CLONE)
    CLONE.parent.mkdir(parents=True, exist_ok=True)

    # Shallow-clone just the pinned tag, then verify it is the pinned commit.
    print(f"cloning {REPO_URL} @ {PIN_TAG} ...")
    run("git", "clone", "--depth", "1", "--branch", PIN_TAG, REPO_URL, str(CLONE))
    head = run("git", "-C", str(CLONE), "rev-parse", "HEAD")
    if head != PIN_HASH:
        _force_rmtree(CLONE)
        sys.exit(
            f"pin mismatch: tag {PIN_TAG} resolved to {head}, expected {PIN_HASH}.\n"
            "Update PIN_TAG/PIN_HASH together after vetting the new commit."
        )

    # Wipe the previously vendored payload so dropped-upstream files do not linger.
    for name in COPY_FILES + COPY_DIRS:
        target = DEST / name
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()

    # Copy the minimal subset we build.
    for name in COPY_DIRS:
        shutil.copytree(CLONE / name, DEST / name, ignore=_ignore_cruft)
    for name in COPY_FILES:
        shutil.copy2(CLONE / name, DEST / name)

    _force_rmtree(CLONE)

    vendored = sorted(
        p.relative_to(DEST).as_posix()
        for p in DEST.rglob("*")
        if p.is_file() and ".clone" not in p.parts
    )

    print(f"\nvendored mimalloc {PIN_TAG} ({PIN_HASH[:12]}): {len(vendored)} files")
    print(f"into {DEST.as_posix()}")
    print(
        "\nremember: our CMakeLists.txt compiles only src/static.c, but the whole\n"
        "src/ tree must be committed for its #includes to resolve."
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
