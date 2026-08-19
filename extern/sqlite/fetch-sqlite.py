#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Download the pinned SQLite amalgamation into extern/sqlite/.install.

SQLite backs babel-serializer's `babel::sqlite` format (a live database handle over the amalgamation).
Unlike mimalloc and xxHash it is fetched-on-demand rather than committed: the amalgamated sqlite3.c is
~9.5 MB of generated C that has no business in our history.
So we follow the SDL3/Zydis model — a gitignored .install/ that dev.py hydrates per configure
(see tools/dev/lib/pipeline/prereqs.py) and CI rebuilds with the same command.

Upstream publishes the amalgamation as a ready-made release zip (sqlite3.c + sqlite3.h + sqlite3ext.h + shell.c),
so unlike Zydis there is no clone/amalgamate step — a plain download+verify (SDL3 idiom) is enough.
We keep only the three files we build against and drop shell.c.
SQLite is public domain and ships no LICENSE file, so there is nothing to copy alongside.

Pinning lives in dependency.yml next to this script: `version`/`year` are the human-readable release, and `pin_hash` is the authority.
It is the SHA3-256 that sqlite.org publishes for the zip (verifying their published digest,
not a digest of our own re-download), and the download is rejected unless it matches.
Bump version, year, asset and pin_hash together after vetting a new release.

Re-running is idempotent: a run whose .install/pin.txt already matches pin_hash is a no-op.
Pass --force to re-download anyway.
"""

import argparse
import hashlib
import io
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

# This script lives in extern/sqlite/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Upstream member (relative to the zip's one top-level directory) -> installed destination (relative to INSTALL).
# sqlite3.c only needs sqlite3.h next to it, but sqlite3ext.h ships in the header set too; shell.c is dropped.
# Pulling members out by name is what keeps a stray path from ever landing outside .install/.
COPY_MAP = {
    "sqlite3.c": "src/sqlite3.c",
    "sqlite3.h": "include/sqlite3.h",
    "sqlite3ext.h": "include/sqlite3ext.h",
}


def already_installed(pin_hash: str) -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == pin_hash


def main() -> int:
    ap = argparse.ArgumentParser(description="Download the pinned SQLite amalgamation.")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    args = ap.parse_args()

    up = deps_manifest.one(DEST)

    # The zip wraps everything in one top-level directory, named after the release.
    strip_prefix = f"sqlite-amalgamation-{up.version}/"

    if not args.force and already_installed(up.pin_hash):
        print(f"sqlite {up.version} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    print(f"downloading {up.name} {up.version} ({up.asset}) ...", flush=True)
    request = urllib.request.Request(up.url, headers={"User-Agent": "shaped-core-sqlite-fetch"})
    with urllib.request.urlopen(request) as response:  # noqa: S310 (pinned sqlite.org release URL)
        data = response.read()

    got = hashlib.sha3_256(data).hexdigest()
    if got != up.pin_hash:
        sys.exit(f"sha3-256 mismatch for {up.asset}: got {got}, expected {up.pin_hash}.\n"
                 "Update version/year/asset/pin_hash together in dependency.yml, after vetting the new release.")

    # Fresh install (drop any prior version).
    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    INSTALL.mkdir(parents=True)

    print(f"extracting to {INSTALL.as_posix()} ...", flush=True)
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for src, dst in COPY_MAP.items():
            dest_path = INSTALL / dst
            dest_path.parent.mkdir(parents=True, exist_ok=True)
            dest_path.write_bytes(archive.read(strip_prefix + src))

    # Written last, so an interrupted run leaves an install that fails already_installed().
    PIN_FILE.write_text(up.pin_hash + "\n", encoding="utf-8")

    print(f"\ninstalled {up.name} {up.version} -> {INSTALL.as_posix()}")
    print("  include/sqlite3.h, include/sqlite3ext.h, src/sqlite3.c")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
