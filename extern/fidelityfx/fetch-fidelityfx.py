#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Download the pinned FSR 3.1 upscaler sources from AMD's FidelityFX SDK into extern/fidelityfx/.install.

These are what `sr::fsr_upscale_routine` builds and runs: AMD's host C++, the HLSL passes, and the headers both include.
A named subset of files rather than the repository, which also carries samples, media, signed DLLs, and parts under AMD's
binary-only license — every file listed in dependency.yml is MIT.

The host code also includes two headers from an `amdinternal/` folder that the public repository does not have.
extern/fidelityfx/CMakeLists.txt copies stand-ins for them from shim/ at configure time, so they are not fetched here.

Re-running is idempotent; pass --force to re-download anyway.
"""

import argparse
import hashlib
import shutil
import sys
import urllib.request
from pathlib import Path

# This script lives in extern/fidelityfx/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

RAW = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/{tag}/{path}"


def fetch(tag: str, entry: dict, into: Path) -> None:
    """Download one pinned file, verify its digest, and place it under `into`."""
    url = RAW.format(tag=tag, path=entry["path"])
    with urllib.request.urlopen(url, timeout=600) as response:
        data = response.read()

    digest = hashlib.sha256(data).hexdigest()
    if digest != entry["sha256"]:
        sys.exit(
            f"{entry['path']}: sha256 {digest} does not match the pin {entry['sha256']}.\n"
            "Refusing to install it. Bump dependency.yml after vetting the new sources."
        )

    target = into / entry["path"]
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true", help="re-download even when the pin already matches")
    args = parser.parse_args()

    # Loading refuses a `pin_hash` that disagrees with `files`, so this pin covers every file below.
    upstream = deps_manifest.one(DEST)
    pin = upstream.pin_hash
    if not pin:
        # `unavailable_on` names this host: CMake then sees no .install/, and sr builds without FSR.
        print("FidelityFX is not pinned for this platform — skipping")
        return

    if not args.force and PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == pin:
        print("FidelityFX already installed")
        return

    # Into a staging directory, moved into place only once every file has arrived and verified.
    # A half-fetched .install/ carrying an old pin.txt would look complete to the CMake check.
    staging = DEST / ".install.staging"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)

    files = upstream.files
    for index, entry in enumerate(files, start=1):
        print(f"  [{index}/{len(files)}] {entry['path']}")
        fetch(upstream.tag, entry, staging)

    (staging / "pin.txt").write_text(pin + "\n", encoding="utf-8")

    # pin.txt goes first, so an rmtree that fails partway leaves no pin claiming a good install.
    PIN_FILE.unlink(missing_ok=True)
    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    staging.rename(INSTALL)

    total = sum(f.stat().st_size for f in INSTALL.rglob("*") if f.is_file())
    print(f"FidelityFX installed into {INSTALL} ({len(files)} files, {total / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
