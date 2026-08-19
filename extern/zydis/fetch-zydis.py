#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
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

# This script lives in extern/zydis/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
CLONE = DEST / ".clone"
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

# Both pins live in dependency.yml next to this script, so no pin is written twice.
# Zycore is a submodule of Zydis, so the pinned Zydis commit already fixes it — we assert it too, so a rewritten submodule pointer cannot slip in unnoticed.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Generated-by-amalgamate.py (relative to CLONE) -> installed destination (relative to INSTALL).
# Zydis.c only `#include <Zydis.h>`, resolved via the include/ dir — mirroring the xxHash layout.
# The two licenses are copied to whatever dependency.yml names, which is what `dev.py deps licenses` then collects.
COPY_MAP = {
    "amalgamated-dist/Zydis.h": "include/Zydis.h",
    "amalgamated-dist/Zydis.c": "src/Zydis.c",
}


def _install_relative(license_file: str) -> str:
    """A `license_files` path is relative to extern/zydis/; the copy loop works relative to .install/."""
    prefix = INSTALL.name + "/"
    if not license_file.startswith(prefix):
        sys.exit(f"dependency.yml: {license_file!r} must live under {prefix}")
    return license_file[len(prefix):]


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


def already_installed(pin_hash: str) -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == pin_hash


def main() -> int:
    ap = argparse.ArgumentParser(description="Fetch and amalgamate the pinned Zydis.")
    ap.add_argument("--force", action="store_true", help="regenerate even if the install is current")
    args = ap.parse_args()

    zydis = deps_manifest.by_name(DEST, "Zydis")
    zycore = deps_manifest.by_name(DEST, "Zycore")

    if not args.force and already_installed(zydis.pin_hash):
        print(f"zydis {zydis.tag} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    # Clean slate: a stale clone or partial previous run must not leak in.
    if CLONE.exists():
        _force_rmtree(CLONE)

    print(f"cloning {zydis.repo} @ {zydis.tag} ...", flush=True)
    run("git", "clone", "--depth", "1", "--branch", zydis.tag, "--recurse-submodules",
        "--shallow-submodules", zydis.repo, str(CLONE))

    # Verify both pins before running any upstream code from the clone.
    head = run("git", "-C", str(CLONE), "rev-parse", "HEAD")
    zycore_head = run("git", "-C", str(CLONE / "dependencies" / "zycore"), "rev-parse", "HEAD")
    for name, got, expected in (("zydis", head, zydis.pin_hash), ("zycore", zycore_head, zycore.pin_hash)):
        if got != expected:
            _force_rmtree(CLONE)
            sys.exit(
                f"pin mismatch: {name} resolved to {got}, expected {expected}.\n"
                "Update both pin_hash values together in dependency.yml, after vetting the new commits."
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

    copies = dict(COPY_MAP)
    copies["LICENSE"] = _install_relative(zydis.license_files[0])
    copies["dependencies/zycore/LICENSE"] = _install_relative(zycore.license_files[0])

    for src, dst in copies.items():
        dest_path = INSTALL / dst
        dest_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(CLONE / src, dest_path)

    _force_rmtree(CLONE)
    PIN_FILE.write_text(zydis.pin_hash + "\n", encoding="utf-8")

    total = sum(p.stat().st_size for p in INSTALL.rglob("*") if p.is_file())
    print(f"\ninstalled {zydis.name} {zydis.tag} ({zydis.pin_hash[:12]}) -> {INSTALL.as_posix()} ({total / 1e6:.1f} MB)")
    print("  include/Zydis.h, src/Zydis.c, LICENSE, LICENSE.zycore")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
