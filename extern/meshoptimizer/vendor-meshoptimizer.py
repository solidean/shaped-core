#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.12"
# dependencies = ["pyyaml>=6"]
# ///
"""Vendor meshoptimizer's decoders in-tree at a pinned commit.

meshoptimizer's decoders are what babel::gltf reads EXT_meshopt_compression with (see the SC_USE_VENDORED_MESHOPTIMIZER option in extern/CMakeLists.txt).
This script shallow-clones the pinned tag into a transient `.clone/` and asserts that the tag resolves to the pinned hash.
It then copies the files we build, and deletes the clone.
The hash is the authority; the tag is a human-readable convenience.

The subset is the decoding half: the vertex codec, the index codec and the vertex filters, plus the one header they share.
The encoders, the optimizers, the simplifier and the rest of the library are dropped, since babel reads meshes and writes no compressed ones.
The header declares the whole library's API, so a call into a dropped part links nowhere rather than compiling to something wrong.

Re-running is idempotent: the previously vendored files are wiped first, so files dropped upstream do not linger.
The vendored payload (include/, src/, LICENSE.md) is committed; this script is only needed to bump or re-vet the version.
"""

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# This script lives in extern/meshoptimizer/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Upstream-relative source -> vendored destination (relative to DEST).
COPY_MAP = {
    "src/meshoptimizer.h": "include/meshoptimizer.h",
    "src/vertexcodec.cpp": "src/vertexcodec.cpp",
    "src/indexcodec.cpp": "src/indexcodec.cpp",
    "src/vertexfilter.cpp": "src/vertexfilter.cpp",
    "LICENSE.md": "LICENSE.md",
}

# Everything we own under DEST that a re-vendor must wipe first.
# The script, CMakeLists.txt and dependency.yml stay.
WIPE = ["include", "src", "LICENSE.md"]


def _force_rmtree(path: Path) -> None:
    """rmtree that survives Windows, where git packs its objects read-only."""

    def on_error(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)

    shutil.rmtree(path, onexc=on_error)


def run(*args: str, cwd: Path | None = None) -> str:
    """Runs a git command and returns its stripped stdout, aborting loudly on failure."""
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"command failed: {' '.join(args)}\n{result.stderr.strip()}")
    return result.stdout.strip()


def main() -> int:
    up = deps_manifest.one(DEST)

    if CLONE.exists():
        _force_rmtree(CLONE)
    CLONE.parent.mkdir(parents=True, exist_ok=True)

    print(f"cloning {up.repo} @ {up.tag} ...")
    run("git", "clone", "--depth", "1", "--branch", up.tag, up.repo, str(CLONE))
    head = run("git", "-C", str(CLONE), "rev-parse", "HEAD")
    if head != up.pin_hash:
        _force_rmtree(CLONE)
        sys.exit(
            f"pin mismatch: tag {up.tag} resolved to {head}, expected {up.pin_hash}.\n"
            "Update tag and pin_hash together in dependency.yml, after vetting the new commit."
        )

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

    _force_rmtree(CLONE)

    vendored = sorted(p.relative_to(DEST).as_posix() for p in DEST.rglob("*") if p.is_file() and ".clone" not in p.parts)
    print(f"\nvendored {up.name} {up.tag} ({up.pin_hash[:12]}): {len(vendored)} files")
    print(f"into {DEST.as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
