#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Vendor xxHash in-tree at a pinned commit.

shaped-core builds offline and reproducibly, so third-party dependencies live
in-tree rather than as submodules or fetched-at-configure-time packages. xxHash
backs cc::hash128 (the XXH3 128-bit hash; see the SC_USE_VENDORED_XXHASH option
in extern/CMakeLists.txt). This script regenerates the vendored copy from a
pinned upstream commit: it shallow-clones the tag into a transient `.clone/`
dir, asserts the tag resolves to the pinned hash (the hash is the authority; the
tag is a human-readable convenience), copies the minimal subset we actually
build, and deletes the clone.

xxHash keeps its sources flat at the repo root, but we mirror the mimalloc
layout (include/ + src/) for consistency: xxhash.h -> include/xxhash.h and
xxhash.c -> src/xxhash.c. Our CMake compiles the single src/xxhash.c TU, which
only `#include "xxhash.h"`, resolved via the include/ dir.

Re-running is idempotent: the previously vendored files are wiped first, so
files dropped upstream do not linger. The vendored payload (include/, src/,
LICENSE) is committed to the repo; this script is only needed to bump or re-vet
the version.
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
REPO_URL = "https://github.com/Cyan4973/xxHash"
PIN_TAG = "v0.8.3"
PIN_HASH = "e626a72bc2321cd320e953a0ccf1584cad60f363"

# This script lives in extern/xxhash/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# Upstream-relative source -> vendored destination (relative to DEST). xxHash's
# sources sit at the repo root; we remap them into include/ + src/ to match the
# mimalloc layout. Everything else (cli/, tests/, build machinery, docs, the
# x86 dispatch variant, ...) is intentionally dropped.
COPY_MAP = {
    "xxhash.h": "include/xxhash.h",
    "xxhash.c": "src/xxhash.c",
    "LICENSE": "LICENSE",
}

# Everything we own under DEST that a re-vendor must wipe first (so a file dropped
# upstream does not linger). The script, CMakeLists.txt, and this list itself stay.
WIPE = ["include", "src", "LICENSE"]


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
    for name in WIPE:
        target = DEST / name
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()

    # Copy the minimal subset we build.
    for src, dst in COPY_MAP.items():
        dest_path = DEST / dst
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(CLONE / src, dest_path)

    _force_rmtree(CLONE)

    vendored = sorted(
        p.relative_to(DEST).as_posix()
        for p in DEST.rglob("*")
        if p.is_file() and ".clone" not in p.parts
    )

    print(f"\nvendored xxHash {PIN_TAG} ({PIN_HASH[:12]}): {len(vendored)} files")
    print(f"into {DEST.as_posix()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
