#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Download the pinned Open Image Denoise weights into extern/oidn-weights/.install.

These are the trained networks behind `sr::denoise_method::oidn`, and they arrive WITHOUT the library that usually
carries them: 1.8 MB of weights rather than 53 MB of runtime, because shaped-rendering runs the network in its own
compute shaders rather than calling Intel's inference.

Apache-2.0, the same license as OIDN itself, so this is fetched on demand like every other permissive dependency.

**The weights come from media.githubusercontent.com, and only the weights.**
The .tza files are Git LFS-tracked and raw.githubusercontent.com serves their POINTER text — 132 bytes that would fail
the digest check here, and would be a nonsense "weights blob" if it somehow passed.
The LICENSE and README beside them are ordinary files, which `media` 404s on, so each entry says which host it wants.
The repository publishes no releases, which is why the pin is a commit rather than a tag.

Re-running is idempotent; pass --force to re-download anyway.
"""

import argparse
import hashlib
import shutil
import sys
import urllib.request
from pathlib import Path

# This script lives in extern/oidn-weights/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Two hosts, because only the weights are LFS-tracked.
# `media` serves LFS content and 404s on an ordinary file; `raw` serves an ordinary file and, for an LFS one, the
# pointer text rather than the bytes — so picking the wrong one fails loudly in one direction and silently in the other.
MEDIA = "https://media.githubusercontent.com/media/RenderKit/oidn-weights/{tag}/{path}"
RAW = "https://raw.githubusercontent.com/RenderKit/oidn-weights/{tag}/{path}"


def pin_over(files: list[dict]) -> str:
    """The digest over the whole set, which is what pin.txt records.

    Over `path sha256` lines rather than over the bytes: it has to change when a file is ADDED or REMOVED, not only
    when one of them changes, and that is exactly what a list of names plus hashes says.
    """
    lines = "".join(f"{f['path']} {f['sha256']}\n" for f in files)
    return hashlib.sha256(lines.encode("utf-8")).hexdigest()


def fetch(tag: str, entry: dict, into: Path) -> None:
    """Download one pinned file, verify its digest, and place it under `into`."""
    host = MEDIA if entry.get("lfs") else RAW
    url = host.format(tag=tag, path=entry["path"])
    with urllib.request.urlopen(url, timeout=600) as response:
        data = response.read()

    digest = hashlib.sha256(data).hexdigest()
    if digest != entry["sha256"]:
        sys.exit(
            f"{entry['path']}: sha256 {digest} does not match the pin {entry['sha256']}.\n"
            "Refusing to install it. Bump dependency.yml after vetting the new weights."
        )

    target = into / entry["path"]
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true", help="re-download even when the pin already matches")
    args = parser.parse_args()

    upstream = deps_manifest.one(DEST)
    files = upstream.files
    pin = pin_over(files)

    # The manifest's own pin must agree with the list it sits above, or the staleness check every other tool runs would
    # be comparing against a number nothing derived.
    if pin != upstream.pin_hash:
        sys.exit(f"dependency.yml: `pin_hash` is {upstream.pin_hash}, but the file list hashes to {pin}.")

    if not args.force and PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == pin:
        print("OIDN weights already installed")
        return

    # Into a staging directory, moved into place only once every file has arrived and verified.
    # A half-fetched .install/ carrying an old pin.txt would look complete to the CMake check.
    staging = DEST / ".install.staging"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)

    for index, entry in enumerate(files, start=1):
        print(f"  [{index}/{len(files)}] {entry['path']}")
        fetch(upstream.tag, entry, staging)

    (staging / "pin.txt").write_text(pin + "\n", encoding="utf-8")

    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    staging.rename(INSTALL)

    total = sum(f.stat().st_size for f in INSTALL.rglob("*") if f.is_file())
    print(f"OIDN weights installed into {INSTALL} ({len(files)} files, {total / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
