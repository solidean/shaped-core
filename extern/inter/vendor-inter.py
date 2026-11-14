#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# dependencies = ["pyyaml>=6"]
# ///
"""Vendor Inter's static Regular TTF and its license in-tree, from a pinned release zip.

A browser page cannot open a system font file, so a web build draws its text in this one.
The script downloads the release asset dependency.yml names, rejects it unless its SHA-256 is the pin, and copies two files out of it.
Those two files are committed; this script is only needed to bump or re-vet the version.

    uv run extern/inter/vendor-inter.py [--archive <Inter-x.y.zip already downloaded>]
"""

import argparse
import hashlib
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

DEST = Path(__file__).resolve().parent
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Archive member -> vendored destination (relative to DEST).
COPY_MAP = {
    "extras/ttf/Inter-Regular.ttf": "Inter-Regular.ttf",
    "LICENSE.txt": "LICENSE.txt",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--archive", type=Path, help="a release zip already downloaded, instead of fetching it")
    args = parser.parse_args()

    up = deps_manifest.one(DEST)
    with tempfile.TemporaryDirectory() as tmp:
        archive = args.archive
        if archive is None:
            archive = Path(tmp) / up.asset
            print(f"downloading {up.url}")
            urllib.request.urlretrieve(up.url, archive)

        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != up.pin_hash:
            sys.exit(f"{archive.name}: sha256 {digest} is not the pinned {up.pin_hash}")

        with zipfile.ZipFile(archive) as z:
            for member, dest in COPY_MAP.items():
                (DEST / dest).write_bytes(z.read(member))
                print(f"{member} -> {dest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
