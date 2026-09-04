#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Vendor Mbed TLS in-tree at a pinned commit.

shaped-core builds offline and reproducibly, so third-party dependencies live
in-tree rather than as submodules or fetched-at-configure-time packages. Mbed TLS
is the TLS handshake and record layer behind clean-net (see the
SC_USE_VENDORED_MBEDTLS option in extern/CMakeLists.txt). This script regenerates
the vendored copy from a pinned upstream commit: it shallow-clones the tag into a
transient `.clone/` dir, asserts the tag resolves to the pinned hash (the hash is
the authority; the tag is a human-readable convenience), copies the subset we
actually build, and deletes the clone.

The subset is include/ and library/ and nothing else. What is dropped is
upstream's programs/, tests/, docs/, doxygen/, scripts/ and build machinery, plus
3rdparty/ -- the Everest X25519 and p256-m accelerators, which the default
configuration does not enable and which shaped_mbedtls_config.h does not turn on.

The 3.6 LTS line rather than 4.x, deliberately: 4.0 moves the crypto half into a
separate tf-psa-crypto repository, which would mean a second pin and a submodule
for nothing we need. 3.6 is self-contained -- including the PSA driver wrappers,
which upstream generates from Jinja templates on the 4.x line but commits on this
one, so vendoring needs no code generation step.

Re-running is idempotent: the previously vendored files are wiped first, so files
dropped upstream do not linger. The vendored payload (include/, library/, LICENSE)
is committed to the repo; this script is only needed to bump or re-vet the
version.
"""

import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# This script lives in extern/mbedtls/ and vendors alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"

# The pin lives in dependency.yml next to this script, so it is written once.
# The tag is for humans; pin_hash is the authority -- the clone is rejected unless the tag resolves to exactly that commit.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Upstream-relative directory -> vendored destination, copied whole but filtered by SUFFIXES.
COPY_DIRS = {
    "include/mbedtls": "include/mbedtls",
    "include/psa": "include/psa",
    "library": "library",
}

# Only the sources and headers: upstream's directories also carry CMakeLists.txt, Makefile and .py helpers
# that we replace with our own CMakeLists.txt.
SUFFIXES = {".c", ".h"}

COPY_FILES = {"LICENSE": "LICENSE"}

# Everything we own under DEST that a re-vendor must wipe first, so a file dropped upstream does not linger.
# The script, CMakeLists.txt, dependency.yml and config/ stay -- config/ is ours, not upstream's.
WIPE = ["include", "library", "LICENSE"]


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

    # The PSA driver wrappers are generated from Jinja templates on the 4.x line and committed on this one.
    # If that ever changes, the build would fail far from here with a missing header, so it is checked at the source.
    wrappers = CLONE / "library/psa_crypto_driver_wrappers.h"
    if not wrappers.is_file():
        _force_rmtree(CLONE)
        sys.exit(
            "library/psa_crypto_driver_wrappers.h is not in the tree.\n"
            "Upstream now generates it, so vendoring needs a code-generation step. Re-vet before vendoring."
        )

    # Wipe the previously vendored payload so dropped-upstream files do not linger.
    for name in WIPE:
        target = DEST / name
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()

    copied = 0
    for src, dst in COPY_DIRS.items():
        source_dir = CLONE / src
        dest_dir = DEST / dst
        dest_dir.mkdir(parents=True, exist_ok=True)
        for path in sorted(source_dir.iterdir()):
            if path.is_file() and path.suffix in SUFFIXES:
                shutil.copy2(path, dest_dir / path.name)
                copied += 1

    for src, dst in COPY_FILES.items():
        shutil.copy2(CLONE / src, DEST / dst)
        copied += 1

    _force_rmtree(CLONE)

    print(f"\nvendored {up.name} {up.tag} ({up.pin_hash[:12]}): {copied} files")
    print(f"into {DEST.as_posix()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
