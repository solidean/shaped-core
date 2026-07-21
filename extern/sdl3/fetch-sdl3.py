#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Download the pinned SDL3 source release into extern/sdl3/.install.

SDL3 backs shaped-rendering's sr::window.
It is fetched as source and built by our own CMake (extern/sdl3/CMakeLists.txt) rather than
downloaded prebuilt.
Upstream ships a prebuilt development package only for Windows and macOS, so a prebuilt-only
integration could never cover Linux.
Building from source is one code path on every platform, and SDL compiles with our toolchain.

The tree is ~50 MB across several thousand files, most of them platform backends we never
compile, so it stays out of the history in a gitignored .install/.

Pinning: PIN_TAG is the human-readable release; PIN_HASH (the asset's SHA-256) is the authority,
and the download is rejected unless it matches.
Bump PIN_TAG/ASSET/PIN_HASH and STRIP_PREFIX together after vetting a new release.

Re-running is idempotent: a re-run whose .install/pin.txt already matches PIN_HASH is a no-op.
Pass --force to re-download anyway.
"""

import argparse
import hashlib
import io
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path, PurePosixPath

# Pinned upstream release. https://github.com/libsdl-org/SDL/releases
# PIN_HASH must keep that exact name.
# tools/dev/lib/pipeline/prereqs.py reads it out of this file to decide whether an install is current,
# without paying to start Python.
REPO_URL = "https://github.com/libsdl-org/SDL"
PIN_TAG = "release-3.4.12"
ASSET = "SDL3-3.4.12.tar.gz"
PIN_HASH = "f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7"
URL = f"{REPO_URL}/releases/download/{PIN_TAG}/{ASSET}"

# The tarball wraps everything in one top-level directory, which we strip.
# That makes .install/ itself the source root — the path extern/sdl3/CMakeLists.txt hands to add_subdirectory.
STRIP_PREFIX = "SDL3-3.4.12/"

# This script lives in extern/sdl3/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"


def already_installed() -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == PIN_HASH


def rerooted_members(archive: tarfile.TarFile):
    """Yield the archive's members re-rooted below STRIP_PREFIX, refusing anything that escapes .install/.

    Done by hand rather than with extractall(filter="data"): that keyword is Python 3.12+ and this
    script supports 3.10.
    """
    for member in archive.getmembers():
        if not member.name.startswith(STRIP_PREFIX):
            sys.exit(f"unexpected archive layout: {member.name!r} is not under {STRIP_PREFIX!r}. "
                     "Update STRIP_PREFIX alongside PIN_TAG/ASSET/PIN_HASH.")

        member.name = member.name[len(STRIP_PREFIX):]
        if not member.name:
            continue  # the stripped top-level directory itself

        path = PurePosixPath(member.name)
        if path.is_absolute() or ".." in path.parts:
            sys.exit(f"refusing to extract {member.name!r} (path traversal)")

        # A source release is files and directories.
        # Anything else is unexpected enough to skip rather than trust.
        if member.isfile() or member.isdir():
            yield member


def main() -> int:
    ap = argparse.ArgumentParser(description="Download the pinned SDL3 source release.")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    args = ap.parse_args()

    if not args.force and already_installed():
        print(f"sdl3 {PIN_TAG} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    print(f"downloading SDL3 {PIN_TAG} ({ASSET}) ...", flush=True)
    request = urllib.request.Request(URL, headers={"User-Agent": "shaped-core-sdl3-fetch"})
    with urllib.request.urlopen(request) as response:  # noqa: S310 (pinned github release URL)
        data = response.read()

    got = hashlib.sha256(data).hexdigest()
    if got != PIN_HASH:
        sys.exit(f"sha256 mismatch for {ASSET}: got {got}, expected {PIN_HASH}.\n"
                 "Update PIN_TAG/ASSET/PIN_HASH together after vetting the new release.")

    # Fresh install (drop any prior version).
    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    INSTALL.mkdir(parents=True)

    print(f"extracting to {INSTALL.as_posix()} ...", flush=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        archive.extractall(INSTALL, members=rerooted_members(archive))

    # Written last, so an interrupted run leaves an install that fails already_installed().
    PIN_FILE.write_text(PIN_HASH + "\n", encoding="utf-8")

    print(f"\ninstalled SDL3 {PIN_TAG} -> {INSTALL.as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
