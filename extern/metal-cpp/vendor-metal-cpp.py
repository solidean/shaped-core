#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Vendor Apple's metal-cpp in-tree at a pinned tag.

shaped-core builds offline and reproducibly, so third-party dependencies live in-tree rather than as submodules or fetched-at-configure-time packages.
metal-cpp is how shaped-graphics-metal speaks to Metal at all — see dependency.yml for why it rather than Objective-C++.

The pin tracks the SDK rather than the newest release: a wrapper newer than the framework on disk declares selectors that do not resolve.
So bumping this is a two-part move — a new Xcode, then the matching `release/metal-cpp_macOS<x>_iOS<x>` tag.

Upstream ships four framework directories and we copy three.
MetalFX is dropped: nothing in the tree includes it, and none of the other three reach it.

Re-running is idempotent: the vendored payload (include/, LICENSE) is wiped first, so files dropped upstream do not linger.
That payload is committed to the repo; this script is only needed to bump or re-vet the version.
"""

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# This script lives in extern/metal-cpp/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Upstream-relative source -> vendored destination (relative to DEST).
#
# The three framework directories move under include/ together and keep their names, because metal-cpp's headers
# include each other by relative path ("../Foundation/Foundation.hpp") — so their layout relative to one another is
# load-bearing, and include/ is what a consumer puts on the search path.
# Everything else upstream ships (MetalFX, SingleHeader, README) is intentionally dropped.
COPY_MAP = {
    "Foundation": "include/Foundation",
    "Metal": "include/Metal",
    "QuartzCore": "include/QuartzCore",
    "LICENSE.txt": "LICENSE",
}

# The vendored payload a re-vendor must wipe first (so a file dropped upstream does not linger).
# The script, CMakeLists.txt, dependency.yml and this list itself stay.
WIPE = ["include", "LICENSE"]


def _force_rmtree(path: Path) -> None:
    """rmtree that survives a read-only .git object, the way the other vendor scripts do."""

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

    # Fetch the pinned tag, then verify HEAD against pin_hash — the tag is how a human names it and the hash is what binds.
    print(f"fetching {up.repo} @ {up.tag} ...")
    run("git", "init", "--quiet", str(CLONE))
    run("git", "-C", str(CLONE), "remote", "add", "origin", up.repo)
    run("git", "-C", str(CLONE), "fetch", "--depth", "1", "--quiet", "origin", f"refs/tags/{up.tag}")
    run("git", "-C", str(CLONE), "checkout", "--quiet", "FETCH_HEAD")
    head = run("git", "-C", str(CLONE), "rev-parse", "HEAD")
    if head != up.pin_hash:
        _force_rmtree(CLONE)
        sys.exit(
            f"pin mismatch: {up.tag} resolved to {head}, expected {up.pin_hash}.\n"
            "Update pin_hash in dependency.yml after vetting the new tag."
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
        source_path = CLONE / src
        dest_path = DEST / dst
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        if source_path.is_dir():
            shutil.copytree(source_path, dest_path)
        else:
            shutil.copy2(source_path, dest_path)

    _force_rmtree(CLONE)

    vendored = sorted(
        p.relative_to(DEST).as_posix()
        for p in DEST.rglob("*")
        if p.is_file() and ".clone" not in p.parts
    )

    print(f"\nvendored {up.name} ({up.pin_hash[:12]}): {len(vendored)} files")
    print(f"into {DEST.as_posix()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
