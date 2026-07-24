#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
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

Pinning: PIN_VERSION/PIN_YEAR are the human-readable release; PIN_HASH is the authority.
It is the SHA3-256 that sqlite.org publishes for the zip (verifying their published digest,
not a digest of our own re-download), and the download is rejected unless it matches.
Bump PIN_VERSION/PIN_YEAR/PIN_HASH together after vetting a new release.

Re-running is idempotent: a run whose .install/pin.txt already matches PIN_HASH is a no-op.
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

# Pinned upstream release. https://sqlite.org/download.html
# PIN_HASH is the SHA3-256 sqlite.org lists for the amalgamation zip (the authority).
# tools/dev/lib/pipeline/prereqs.py reads PIN_HASH out of this file to decide whether an install
# is current, without paying to start Python — keep the `PIN_HASH = "..."` assignment shape.
PIN_VERSION = "3530300"  # SQLite 3.53.3
PIN_YEAR = "2026"
ASSET = f"sqlite-amalgamation-{PIN_VERSION}.zip"
PIN_HASH = "d45c688a8cb23f68611a894a756a12d7eb6ab6e9e2468ca70adbeab3808b5ab9"
URL = f"https://sqlite.org/{PIN_YEAR}/{ASSET}"

# The zip wraps everything in one top-level directory.
# We pull specific members out by name, so a stray path never lands outside .install/.
STRIP_PREFIX = f"sqlite-amalgamation-{PIN_VERSION}/"

# Upstream member (inside the zip) -> installed destination (relative to INSTALL).
# sqlite3.c only needs sqlite3.h next to it, but sqlite3ext.h ships in the header set too; shell.c is dropped.
COPY_MAP = {
    f"{STRIP_PREFIX}sqlite3.c": "src/sqlite3.c",
    f"{STRIP_PREFIX}sqlite3.h": "include/sqlite3.h",
    f"{STRIP_PREFIX}sqlite3ext.h": "include/sqlite3ext.h",
}

# This script lives in extern/sqlite/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"


def already_installed() -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == PIN_HASH


def main() -> int:
    ap = argparse.ArgumentParser(description="Download the pinned SQLite amalgamation.")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    args = ap.parse_args()

    if not args.force and already_installed():
        print(f"sqlite {PIN_VERSION} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    print(f"downloading SQLite {PIN_VERSION} ({ASSET}) ...", flush=True)
    request = urllib.request.Request(URL, headers={"User-Agent": "shaped-core-sqlite-fetch"})
    with urllib.request.urlopen(request) as response:  # noqa: S310 (pinned sqlite.org release URL)
        data = response.read()

    got = hashlib.sha3_256(data).hexdigest()
    if got != PIN_HASH:
        sys.exit(f"sha3-256 mismatch for {ASSET}: got {got}, expected {PIN_HASH}.\n"
                 "Update PIN_VERSION/PIN_YEAR/PIN_HASH together after vetting the new release.")

    # Fresh install (drop any prior version).
    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    INSTALL.mkdir(parents=True)

    print(f"extracting to {INSTALL.as_posix()} ...", flush=True)
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        for src, dst in COPY_MAP.items():
            dest_path = INSTALL / dst
            dest_path.parent.mkdir(parents=True, exist_ok=True)
            dest_path.write_bytes(archive.read(src))

    # Written last, so an interrupted run leaves an install that fails already_installed().
    PIN_FILE.write_text(PIN_HASH + "\n", encoding="utf-8")

    print(f"\ninstalled SQLite {PIN_VERSION} -> {INSTALL.as_posix()}")
    print("  include/sqlite3.h, include/sqlite3ext.h, src/sqlite3.c")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
