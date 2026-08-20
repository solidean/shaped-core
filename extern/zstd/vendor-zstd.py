#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Vendor Zstandard in-tree at a pinned commit.

shaped-core builds offline and reproducibly, so third-party dependencies live
in-tree rather than as submodules or fetched-at-configure-time packages. zstd
backs one half of cc::compress / cc::decompress (see the SC_USE_VENDORED_ZSTD
option in extern/CMakeLists.txt). This script regenerates the vendored copy from
a pinned upstream commit: it shallow-clones the tag into a transient `.clone/`
dir, asserts the tag resolves to the pinned hash (the hash is the authority; the
tag is a human-readable convenience), copies the minimal subset we actually
build, and deletes the clone.

Unlike the other vendored deps here, this one does NOT remap into include/ +
src/. zstd's sources reach each other with relative quote includes — a compress
TU says `#include "../common/mem.h"` and `#include "../zstd.h"` — so the
directory shape IS part of the source. src/ therefore mirrors upstream's lib/
verbatim, and CMake exposes src/ itself as the public include dir. That works
out exactly right: the only headers sitting directly in src/ are zstd.h,
zstd_errors.h and zdict.h, which is precisely the public API, while everything
internal stays one level down in common/ / compress/ / decompress/ and is
unreachable from outside. It also means zstd's own bundled common/xxhash.h can
never shadow the separately vendored extern/xxhash/include/xxhash.h.

That bundled xxhash is kept rather than pointed at extern/xxhash: coupling two
independently pinned upstreams so that bumping one can break the other is a bad
trade for the ~256 kB it would save.

What is dropped, and why:
  legacy/     — decoders for the v0.1-v0.7 formats nobody here has ever written
  deprecated/ — the zbuff API, superseded by the streaming API we use
  dll/        — MSVC solution files for building a shared library
  build files — Makefile, BUCK, .mk, .pc.in, module.modulemap, README

Re-running is idempotent: the previously vendored files are wiped first, so
files dropped upstream do not linger. The vendored payload (src/, LICENSE) is
committed to the repo; this script is only needed to bump or re-vet the version.
"""

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# This script lives in extern/zstd/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# The pin lives in dependency.yml next to this script, so it is written once.
# The tag is for humans; pin_hash is the authority — the clone is rejected unless the tag resolves to exactly that commit.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Single files, upstream-relative source -> vendored destination (relative to DEST).
COPY_MAP = {
    "lib/zstd.h": "src/zstd.h",
    "lib/zstd_errors.h": "src/zstd_errors.h",
    "lib/zdict.h": "src/zdict.h",
    # The BSD-3-Clause text. COPYING, the GPL-2.0 alternative, is deliberately not vendored — see dependency.yml.
    "LICENSE": "LICENSE",
}

# Whole directories, copied with their internal structure intact because the sources depend on it.
# Only these suffixes come across, which is what keeps each directory's Makefile fragments and READMEs out.
COPY_TREES = {
    "lib/common": "src/common",
    "lib/compress": "src/compress",
    "lib/decompress": "src/decompress",
    "lib/dictBuilder": "src/dictBuilder",
}
TREE_SUFFIXES = {".c", ".h", ".S"}

# Everything we own under DEST that a re-vendor must wipe first (so a file dropped
# upstream does not linger). The script, CMakeLists.txt, and this list itself stay.
WIPE = ["src", "LICENSE"]


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
    up = deps_manifest.one(DEST)

    # Clean slate: a stale clone or partial previous run must not leak in.
    if CLONE.exists():
        _force_rmtree(CLONE)
    CLONE.parent.mkdir(parents=True, exist_ok=True)

    # Shallow-clone just the pinned tag, then verify it is the pinned commit.
    print(f"cloning {up.repo} @ {up.tag} ...")
    run("git", "clone", "--depth", "1", "--branch", up.tag, up.repo, str(CLONE))
    head = run("git", "-C", str(CLONE), "rev-parse", "HEAD")
    if head != up.pin_hash:
        _force_rmtree(CLONE)
        sys.exit(
            f"pin mismatch: tag {up.tag} resolved to {head}, expected {up.pin_hash}.\n"
            "Update tag and pin_hash together in dependency.yml, after vetting the new commit."
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

    for src_dir, dst_dir in COPY_TREES.items():
        for src_path in sorted((CLONE / src_dir).rglob("*")):
            if not src_path.is_file() or src_path.suffix not in TREE_SUFFIXES:
                continue
            dest_path = DEST / dst_dir / src_path.relative_to(CLONE / src_dir)
            dest_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src_path, dest_path)

    _force_rmtree(CLONE)

    vendored = sorted(
        p.relative_to(DEST).as_posix()
        for p in DEST.rglob("*")
        if p.is_file() and ".clone" not in p.parts
    )

    print(f"\nvendored {up.name} {up.tag} ({up.pin_hash[:12]}): {len(vendored)} files")
    print(f"into {DEST.as_posix()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
