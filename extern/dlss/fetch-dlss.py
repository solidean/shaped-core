#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Download the pinned NVIDIA DLSS SDK files into extern/dlss/.install.

DLSS backs shaped-rendering's `sr::dlss_rr_routine`, the Ray Reconstruction denoise member.

**Nothing runs this for you.**
Every other fetched dependency is run by dev.py per configure (tools/dev/lib/pipeline/prereqs.py); this one is not, and
deliberately.
The SDK is proprietary — its license is NVIDIA's own rather than an OSI one — so fetching it is a decision a person
makes, not one a build makes on their behalf.
Without it `SR_HAS_DLSS` is 0, `sr::denoise_method::dlss_rr` reports `unsupported`, and everything else builds.

Only what a dx12 Ray Reconstruction integration needs is fetched, rather than the repository: the headers, the import
library, and nvngx_dlssd.dll.
The whole tree carries super-resolution and frame-generation runtimes for five platforms, which is several hundred
megabytes of things we never call.
The one file that is large is nvngx_dlssd.dll at 48 MB, which is redistributable and has to sit next to the binary
that loads it.

Pinning lives in dependency.yml next to this script: `tag` is the release, and every file carries its own sha256, so a
download is rejected unless each one matches.
`pin_hash` is the digest over that whole list, which is what .install/pin.txt records — so a changed file list is a
stale install even when every individual hash still checks out.

Re-running is idempotent: a re-run whose .install/pin.txt already matches is a no-op.
Pass --force to re-download anyway.
"""

import argparse
import hashlib
import shutil
import sys
import urllib.request
from pathlib import Path

# This script lives in extern/dlss/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

RAW = "https://raw.githubusercontent.com/NVIDIA/DLSS/{tag}/{path}"


def pin_over(files: list[dict]) -> str:
    """The digest over the whole file list, which is what pin.txt records.

    Over `path sha256` lines rather than over the bytes: it has to change when a file is ADDED or REMOVED, not only
    when one of them changes, and that is exactly what a list of names plus hashes says.
    """
    lines = "".join(f"{f['path']} {f['sha256']}\n" for f in files)
    return hashlib.sha256(lines.encode("utf-8")).hexdigest()


def already_installed(pin: str) -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == pin


def fetch(tag: str, entry: dict, into: Path) -> None:
    """Download one pinned file, verify its digest, and place it under `into`."""
    url = RAW.format(tag=tag, path=entry["path"])
    with urllib.request.urlopen(url, timeout=600) as response:
        data = response.read()

    digest = hashlib.sha256(data).hexdigest()
    if digest != entry["sha256"]:
        sys.exit(
            f"{entry['path']}: sha256 {digest} does not match the pin {entry['sha256']}.\n"
            "Refusing to install it. Bump dependency.yml after vetting the new release."
        )

    target = into / entry["path"]
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true", help="re-download even when the pin already matches")
    args = parser.parse_args()

    upstream = deps_manifest.one(DEST)
    tag = upstream.tag
    files = upstream.files
    pin = pin_over(files)

    # The manifest's own pin must agree with the list it sits above, or the staleness check every other tool runs would
    # be comparing against a number nothing derived.
    if pin != upstream.pin_hash:
        sys.exit(
            f"dependency.yml: pin_hash is {upstream.pin_hash}, but the file list digests to {pin}.\n"
            "Update pin_hash alongside the list."
        )

    if not args.force and already_installed(pin):
        print(f"DLSS {upstream.version} already installed")
        return

    # Into a staging directory, moved into place only once every file has arrived and verified.
    # A half-fetched .install/ that still carried an old pin.txt would look complete to the CMake check.
    staging = DEST / ".install.staging"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)

    total = len(files)
    for index, entry in enumerate(files, start=1):
        print(f"  [{index}/{total}] {entry['path']}")
        fetch(tag, entry, staging)

    (staging / "pin.txt").write_text(pin + "\n", encoding="utf-8")

    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    staging.rename(INSTALL)

    print(f"DLSS {upstream.version} installed into {INSTALL}")
    print("Configure again to pick it up: uv run dev.py configure")


if __name__ == "__main__":
    main()
