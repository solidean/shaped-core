#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Vendor LZ4 in-tree at a pinned commit.

shaped-core builds offline and reproducibly, so third-party dependencies live
in-tree rather than as submodules or fetched-at-configure-time packages. LZ4
backs one half of cc::compress / cc::decompress (see the SC_USE_VENDORED_LZ4
option in extern/CMakeLists.txt). This script regenerates the vendored copy from
a pinned upstream commit: it shallow-clones the tag into a transient `.clone/`
dir, asserts the tag resolves to the pinned hash (the hash is the authority; the
tag is a human-readable convenience), copies the minimal subset we actually
build, and deletes the clone.

Only `lib/` is copied, and that is a LICENSE boundary rather than a size one:
`lib/` is BSD-2-Clause while the rest of the repository is GPL-2.0-or-later.
Within it we take the block codec (lz4), the high-compression codec (lz4hc), the
frame layer (lz4frame) and the xxhash copy lz4frame checksums with. `lz4file` is
dropped — it wraps stdio FILE*, and streaming here goes through cc::read_stream.

Upstream keeps its sources flat in lib/, and we split them into include/ + src/
to match the mimalloc layout. Only the four public headers land in include/;
lz4's own xxhash.h stays in src/, so it can never shadow the separately
vendored extern/xxhash/include/xxhash.h that cc::hash128 includes.

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

# This script lives in extern/lz4/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# The pin lives in dependency.yml next to this script, so it is written once.
# The tag is for humans; pin_hash is the authority — the clone is rejected unless the tag resolves to exactly that commit.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Upstream-relative source -> vendored destination (relative to DEST).
# lz4hc.c does `#include "lz4.c"` under LZ4_COMMONDEFS_ONLY, which is how upstream builds it too — so both .c files sit
# in src/ where that quote-include resolves, and both are compiled as ordinary TUs.
# Everything else (programs/, tests/, examples/, build machinery, docs, lz4file's stdio wrapper) is intentionally dropped.
COPY_MAP = {
    "lib/lz4.h": "include/lz4.h",
    "lib/lz4hc.h": "include/lz4hc.h",
    "lib/lz4frame.h": "include/lz4frame.h",
    "lib/lz4frame_static.h": "include/lz4frame_static.h",
    "lib/lz4.c": "src/lz4.c",
    "lib/lz4hc.c": "src/lz4hc.c",
    "lib/lz4frame.c": "src/lz4frame.c",
    "lib/xxhash.h": "src/xxhash.h",
    "lib/xxhash.c": "src/xxhash.c",
    # lib/LICENSE, not the repo-root one: the root file only explains the split, this is the BSD-2-Clause text itself.
    "lib/LICENSE": "LICENSE",
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
