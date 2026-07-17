#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Fetch the pinned Zydis into extern/zydis/.install as an amalgamated single-TU source.

Zydis is the x86-64 instruction decoder behind tools/instruction-tracer. Unlike mimalloc
and xxHash it is fetched-on-demand rather than committed: upstream's amalgamation folds
Zydis + Zycore into one Zydis.h and one ~12 MB Zydis.c (mostly generated instruction
tables), which has no business in our history for a tool that is optional, dev-only and
Windows-only. So we follow the DXC model instead — a gitignored .install/ that dev.py
hydrates per configure (see tools/dev/lib/pipeline/prereqs.py) and CI rebuilds with the
same command.

Upstream publishes no amalgamated release asset, so we generate it: shallow-clone the
pinned tag (with the zycore submodule), run upstream's assets/amalgamate.py, and keep only
Zydis.h + Zydis.c + both licenses. extern/zydis/CMakeLists.txt then builds the single
src/Zydis.c TU into a static `zydis` target, the same shape as the vendored xxHash.

Pinning: the tags are for humans; PIN_HASH / ZYCORE_PIN_HASH are the authority — the clone
is rejected unless both resolve exactly. Bump tag and hash together, only after vetting the
new commit. (The generated files cannot be content-hashed: amalgamate.py stamps the clone's
absolute paths into its comments, so the output is machine-dependent. The commit pins both
the inputs and the amalgamation script itself, which is what actually matters.)

Re-running is idempotent: a run whose .install/pin.txt already matches PIN_HASH is a no-op.
Pass --force to regenerate anyway.
"""

import argparse
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

# Pinned upstream. https://github.com/zyantific/zydis/releases
REPO_URL = "https://github.com/zyantific/zydis"
PIN_TAG = "v4.1.1"
PIN_HASH = "a2278f1d254e492f6a6b39f6cb5d1f5d515659dc"

# Zycore is a submodule of Zydis; the pinned Zydis commit already fixes it, but we assert it
# too so a rewritten submodule pointer cannot slip in unnoticed.
ZYCORE_PIN_HASH = "0b2432ced0884fd152b471d97ecf0258ff4d859f"

# This script lives in extern/zydis/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

# Generated-by-amalgamate.py (relative to CLONE) -> installed destination (relative to INSTALL).
# Zydis.c only `#include <Zydis.h>`, resolved via the include/ dir — mirroring the xxHash layout.
COPY_MAP = {
    "amalgamated-dist/Zydis.h": "include/Zydis.h",
    "amalgamated-dist/Zydis.c": "src/Zydis.c",
    "LICENSE": "LICENSE",
    "dependencies/zycore/LICENSE": "LICENSE.zycore",
}


def _force_rmtree(path: Path) -> None:
    """rmtree that survives Windows: git packs the .git objects read-only, which
    blocks os.unlink — clear the read-only bit on error and retry."""

    def on_error(func, p, _exc):
        os.chmod(p, stat.S_IWRITE)
        func(p)

    shutil.rmtree(path, onexc=on_error)


def run(*args: str, cwd: Path | None = None) -> str:
    """Run a command, returning stripped stdout; abort loudly on failure."""
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"command failed: {' '.join(args)}\n{result.stderr.strip()}")
    return result.stdout.strip()


def already_installed() -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == PIN_HASH


def main() -> int:
    ap = argparse.ArgumentParser(description="Fetch and amalgamate the pinned Zydis.")
    ap.add_argument("--force", action="store_true", help="regenerate even if the install is current")
    args = ap.parse_args()

    if not args.force and already_installed():
        print(f"zydis {PIN_TAG} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    # Clean slate: a stale clone or partial previous run must not leak in.
    if CLONE.exists():
        _force_rmtree(CLONE)

    print(f"cloning {REPO_URL} @ {PIN_TAG} ...", flush=True)
    run("git", "clone", "--depth", "1", "--branch", PIN_TAG, "--recurse-submodules",
        "--shallow-submodules", REPO_URL, str(CLONE))

    # Verify both pins before running any upstream code from the clone.
    head = run("git", "-C", str(CLONE), "rev-parse", "HEAD")
    zycore_head = run("git", "-C", str(CLONE / "dependencies" / "zycore"), "rev-parse", "HEAD")
    for name, got, expected in (("zydis", head, PIN_HASH), ("zycore", zycore_head, ZYCORE_PIN_HASH)):
        if got != expected:
            _force_rmtree(CLONE)
            sys.exit(
                f"pin mismatch: {name} resolved to {got}, expected {expected}.\n"
                "Update the PIN_* hashes together after vetting the new commits."
            )

    # Fold Zydis + Zycore into amalgamated-dist/{Zydis.h,Zydis.c}.
    print("amalgamating ...", flush=True)
    amalgamate = CLONE / "assets" / "amalgamate.py"
    result = subprocess.run([sys.executable, str(amalgamate)], cwd=CLONE, capture_output=True, text=True)
    if result.returncode != 0:
        _force_rmtree(CLONE)
        sys.exit(f"amalgamate.py failed:\n{result.stdout.strip()}\n{result.stderr.strip()}")

    # Fresh install (drop any prior version).
    if INSTALL.exists():
        _force_rmtree(INSTALL)

    for src, dst in COPY_MAP.items():
        dest_path = INSTALL / dst
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(CLONE / src, dest_path)

    _force_rmtree(CLONE)
    PIN_FILE.write_text(PIN_HASH + "\n", encoding="utf-8")

    total = sum(p.stat().st_size for p in INSTALL.rglob("*") if p.is_file())
    print(f"\ninstalled Zydis {PIN_TAG} ({PIN_HASH[:12]}) -> {INSTALL.as_posix()} ({total / 1e6:.1f} MB)")
    print("  include/Zydis.h, src/Zydis.c, LICENSE, LICENSE.zycore")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
