#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Vendor stb (stb_image + stb_image_write) in-tree at a pinned commit.

shaped-core builds offline and reproducibly, so third-party dependencies live in-tree rather than as submodules or fetched-at-configure-time packages.
stb backs babel-serializer's image codecs (see the SC_USE_VENDORED_STB option in extern/CMakeLists.txt).
This script regenerates the vendored copy from a pinned upstream commit: it fetches exactly that commit into a transient `.clone/` dir, asserts HEAD is the pinned hash, copies the two headers + LICENSE, and deletes the clone.

Two differences from vendor-xxhash.py, both from how stb ships:

- stb publishes NO release tags — its canonical repo is a rolling `master`.
  So the pin is a commit hash only (no PIN_TAG), fetched by SHA rather than by tag; PIN_HASH is the sole authority.
- src/stb.c is OURS, not upstream.
  Upstream keeps the implementation behind STB_*_IMPLEMENTATION macros inside the headers; our src/stb.c defines those macros and `#include`s them.
  So it is authored here, never copied or wiped — WIPE covers only the vendored payload (include/ + LICENSE).

Pinned versions: stb_image v2.30, stb_image_write v1.16.

Re-running is idempotent: the vendored payload (include/, LICENSE) is wiped first, so files dropped upstream do not linger.
That payload is committed to the repo; this script is only needed to bump or re-vet the version.
"""

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# Pinned upstream. stb has no tags, so PIN_HASH is the only pin — the fetch is rejected unless HEAD resolves to exactly this commit.
# Bump it only after vetting the new commit (stb is a rolling master; there is no version tag to track).
REPO_URL = "https://github.com/nothings/stb"
PIN_HASH = "31c1ad37456438565541f4919958214b6e762fb4"

# This script lives in extern/stb/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# Upstream-relative source -> vendored destination (relative to DEST).
# stb keeps its sources flat at the repo root; we remap the two headers into include/ and copy the license.
# Everything else (the other stb_*.h, tests, docs, tooling) is intentionally dropped.
# src/stb.c is NOT here — it is our impl TU, not upstream's.
COPY_MAP = {
    "stb_image.h": "include/stb_image.h",
    "stb_image_write.h": "include/stb_image_write.h",
    "LICENSE": "LICENSE",
}

# The vendored payload a re-vendor must wipe first (so a file dropped upstream does not linger).
# src/ is deliberately excluded: src/stb.c is ours.
# The script, CMakeLists.txt, src/stb.c, and this list itself stay.
WIPE = ["include", "LICENSE"]


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

    # Fetch exactly the pinned commit by SHA (stb has no tags), then verify HEAD.
    # GitHub serves any reachable commit SHA to `git fetch`, so a shallow fetch of the single pinned commit is enough — no full history, no branch checkout.
    print(f"fetching {REPO_URL} @ {PIN_HASH} ...")
    run("git", "init", "--quiet", str(CLONE))
    run("git", "-C", str(CLONE), "remote", "add", "origin", REPO_URL)
    run("git", "-C", str(CLONE), "fetch", "--depth", "1", "--quiet", "origin", PIN_HASH)
    run("git", "-C", str(CLONE), "checkout", "--quiet", "FETCH_HEAD")
    head = run("git", "-C", str(CLONE), "rev-parse", "HEAD")
    if head != PIN_HASH:
        _force_rmtree(CLONE)
        sys.exit(
            f"pin mismatch: fetch resolved to {head}, expected {PIN_HASH}.\n"
            "Update PIN_HASH after vetting the new commit."
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

    print(f"\nvendored stb ({PIN_HASH[:12]}): {len(vendored)} files")
    print(f"into {DEST.as_posix()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
