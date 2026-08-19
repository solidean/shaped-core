#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Vendor the BLAKE3 C implementation in-tree at a pinned commit.

shaped-core builds offline and reproducibly, so third-party dependencies live
in-tree rather than as submodules or fetched-at-configure-time packages. BLAKE3
backs cc::blake3 and cc::hash256, the cryptographic hash content addressing
needs (see the SC_USE_VENDORED_BLAKE3 option in extern/CMakeLists.txt). This
script regenerates the vendored copy from a pinned upstream commit: it
shallow-clones the tag into a transient `.clone/` dir, asserts the tag resolves
to the pinned hash (the hash is the authority; the tag is a human-readable
convenience), copies the minimal subset we actually build, and deletes the
clone.

Upstream keeps the C implementation under `c/`; we remap it into the mimalloc
layout (include/ + src/) for consistency: c/blake3.h -> include/blake3.h and
every compiled TU plus the internal c/blake3_impl.h -> src/. Our CMake puts
both dirs on the include path, because blake3.c includes "blake3_impl.h" which
in turn includes "blake3.h".

We vendor the SIMD *intrinsics* TUs and none of the hand-written assembly, so
the build stays a pure C build with no ASM language enabled. Upstream's own
blake3_dispatch.c still selects the ISA at runtime; extern/blake3/CMakeLists.txt
decides which TUs exist and defines the matching BLAKE3_NO_* for the rest.

Re-running is idempotent: the previously vendored files are wiped first, so
files dropped upstream do not linger. The vendored payload (include/, src/, the
licenses) is committed to the repo; this script is only needed to bump or
re-vet the version.
"""

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# This script lives in extern/blake3/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Upstream-relative source -> vendored destination (relative to DEST). Everything
# else (the Rust crate, b3sum, the assembly variants, the TBB backend, upstream's
# own CMake/Make machinery, tests, docs, ...) is intentionally dropped.
#
# BLAKE3 is triple-licensed and lets the user pick; all three texts are copied so
# that choice stays available to whoever consumes shaped-core.
COPY_MAP = {
    "c/blake3.h": "include/blake3.h",
    "c/blake3_impl.h": "src/blake3_impl.h",
    "c/blake3.c": "src/blake3.c",
    "c/blake3_dispatch.c": "src/blake3_dispatch.c",
    "c/blake3_portable.c": "src/blake3_portable.c",
    "c/blake3_sse2.c": "src/blake3_sse2.c",
    "c/blake3_sse41.c": "src/blake3_sse41.c",
    "c/blake3_avx2.c": "src/blake3_avx2.c",
    "c/blake3_avx512.c": "src/blake3_avx512.c",
    "c/blake3_neon.c": "src/blake3_neon.c",
    "LICENSE_A2": "LICENSE_A2",
    "LICENSE_A2LLVM": "LICENSE_A2LLVM",
    "LICENSE_CC0": "LICENSE_CC0",
}

# Everything we own under DEST that a re-vendor must wipe first (so a file dropped
# upstream does not linger). The script, CMakeLists.txt, and this list itself stay.
WIPE = ["include", "src", "LICENSE_A2", "LICENSE_A2LLVM", "LICENSE_CC0"]


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
