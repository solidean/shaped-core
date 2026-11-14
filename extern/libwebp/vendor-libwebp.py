#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# dependencies = ["pyyaml>=6"]
# ///
"""Vendor libwebp in-tree at a pinned commit.

libwebp is the WebP codec behind babel::image's webp format (see SC_USE_VENDORED_LIBWEBP in extern/CMakeLists.txt).
This script shallow-clones the pinned tag into a transient `.clone/`, asserts it resolves to the pinned commit,
copies the subset we build, and deletes the clone.

The subset is the still-image codec: src/dec, src/enc, src/dsp, src/utils, the public headers in src/webp, and
sharpyuv (the encoder's sharp RGB-to-YUV conversion, which src/enc reaches directly).
Dropped: src/mux and src/demux (animation and metadata containers), every tool, test and build system, and the
MIPS and MSA dsp variants, which compile to nothing on the architectures we target.

The sources include each other repository-relative ("src/dsp/dsp.h", "sharpyuv/sharpyuv.h"), so both trees keep
their upstream paths under extern/libwebp/ rather than being split into include/ + src/.

Re-running is idempotent: the previously vendored files are wiped first, so files dropped upstream do not linger.
"""

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# This script lives in extern/libwebp/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# The pin lives in dependency.yml next to this script, so it is written once.
# The tag is for humans; pin_hash is the authority — the clone is rejected unless the tag resolves to exactly that commit.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Single files, upstream-relative source -> vendored destination (relative to DEST).
COPY_MAP = {
    "COPYING": "COPYING",
    "PATENTS": "PATENTS",
}

# Whole directories, copied with their structure intact because the sources include each other by these paths.
COPY_TREES = ["src/dec", "src/enc", "src/dsp", "src/utils", "src/webp", "sharpyuv"]
TREE_SUFFIXES = {".c", ".h"}

# dsp variants for architectures we do not build: each is wholly inside its own WEBP_USE_* guard.
SKIP_MARKERS = ("_mips32", "_mips_dsp_r2", "_msa")

# The public headers of the containers we drop, which nothing in the codec includes.
# mux_types.h stays: the decoder reads its WebPData.
SKIP_FILES = {"src/webp/mux.h", "src/webp/demux.h"}

# Everything we own under DEST that a re-vendor must wipe first (so a file dropped
# upstream does not linger). The script, CMakeLists.txt, and dependency.yml stay.
WIPE = ["src", "sharpyuv", "COPYING", "PATENTS"]


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

    for src, dst in COPY_MAP.items():
        dest_path = DEST / dst
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(CLONE / src, dest_path)

    for tree in COPY_TREES:
        for src_path in sorted((CLONE / tree).rglob("*")):
            rel = src_path.relative_to(CLONE).as_posix()
            if not src_path.is_file() or src_path.suffix not in TREE_SUFFIXES:
                continue
            if rel in SKIP_FILES or any(m in src_path.stem for m in SKIP_MARKERS):
                continue
            dest_path = DEST / rel
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
