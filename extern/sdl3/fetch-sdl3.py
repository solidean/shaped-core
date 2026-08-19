#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
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

Pinning lives in dependency.yml next to this script: `tag` is the human-readable release, and `pin_hash` (the asset's SHA-256) is the authority,
so the download is rejected unless it matches.
Bump tag, asset, version and pin_hash together after vetting a new release — the archive's top-level directory is built from `version`.

Re-running is idempotent: a re-run whose .install/pin.txt already matches pin_hash is a no-op.
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

# This script lives in extern/sdl3/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402


def already_installed(pin_hash: str) -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == pin_hash


def rerooted_members(archive: tarfile.TarFile, strip_prefix: str):
    """Yield the archive's members re-rooted below `strip_prefix`, refusing anything that escapes .install/.

    Done by hand rather than with extractall(filter="data"): that keyword is Python 3.12+ and this
    script supports 3.10.
    """
    for member in archive.getmembers():
        if not member.name.startswith(strip_prefix):
            sys.exit(f"unexpected archive layout: {member.name!r} is not under {strip_prefix!r}. "
                     "The prefix is built from `version` in dependency.yml; update it alongside tag/asset/pin_hash.")

        member.name = member.name[len(strip_prefix):]
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

    up = deps_manifest.one(DEST)

    # The tarball wraps everything in one top-level directory, which we strip.
    # That makes .install/ itself the source root — the path extern/sdl3/CMakeLists.txt hands to add_subdirectory.
    strip_prefix = f"SDL3-{up.version}/"

    if not args.force and already_installed(up.pin_hash):
        print(f"sdl3 {up.tag} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    print(f"downloading {up.name} {up.tag} ({up.asset}) ...", flush=True)
    request = urllib.request.Request(up.url, headers={"User-Agent": "shaped-core-sdl3-fetch"})
    with urllib.request.urlopen(request) as response:  # noqa: S310 (pinned github release URL)
        data = response.read()

    got = hashlib.sha256(data).hexdigest()
    if got != up.pin_hash:
        sys.exit(f"sha256 mismatch for {up.asset}: got {got}, expected {up.pin_hash}.\n"
                 "Update tag/asset/pin_hash together in dependency.yml, after vetting the new release.")

    # Fresh install (drop any prior version).
    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    INSTALL.mkdir(parents=True)

    print(f"extracting to {INSTALL.as_posix()} ...", flush=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        archive.extractall(INSTALL, members=rerooted_members(archive, strip_prefix))

    # Written last, so an interrupted run leaves an install that fails already_installed().
    PIN_FILE.write_text(up.pin_hash + "\n", encoding="utf-8")

    print(f"\ninstalled {up.name} {up.tag} -> {INSTALL.as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
