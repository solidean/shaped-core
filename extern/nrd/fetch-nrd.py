#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Download the pinned NRD sources into extern/nrd/.install.

NRD backs shaped-rendering's `sr::nrd_denoise_routine`, the vendor-neutral split-signal denoise member.

**Nothing runs this for you**, exactly as with DLSS: the license is NVIDIA's own — the RTX SDKs license names the NRD
SDK alongside the DLSS one — so fetching it is a decision a person makes rather than one a build makes on their behalf.
Without it `SR_HAS_NRD` is 0, `sr::denoise_method::nrd` reports `unsupported`, and everything else builds.

THREE archives, because NRD's own CMake fetches two of them over the network at configure time.
Pinning ShaderMake and MathLib here and pre-seeding FetchContent with them is what keeps our configure offline and
reproducible — see extern/nrd/CMakeLists.txt, which also points ShaderMake at the dxc.exe extern/dxc pins rather than
letting it download a second compiler.

Each archive is verified against its own sha256, and `.install/pin.txt` records the digest over all three, so a changed
set is a stale install even when every individual hash still checks out.

Re-running is idempotent; pass --force to re-download anyway.
"""

import argparse
import hashlib
import io
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

# This script lives in extern/nrd/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

# The pins live in dependency.yml next to this script, so they are written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Where each upstream's archive is unpacked, under .install/.
# Named for the upstream rather than for the archive's own top directory, which carries a version we would then have to
# spell in the CMake as well.
SUBDIR = {"NRD": "NRD", "ShaderMake": "ShaderMake", "MathLib": "MathLib"}


def pin_over(upstreams: list) -> str:
    """The digest over the whole set, which is what pin.txt records."""
    lines = "".join(f"{u.name} {u.pin_hash}\n" for u in upstreams)
    return hashlib.sha256(lines.encode("utf-8")).hexdigest()


def already_installed(pin: str) -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == pin


def url_of(upstream) -> str:
    """Where this upstream's archive lives.

    A tag release and a bare commit are different URL shapes on GitHub, and `tag` carries either — a 40-character hex
    string is the commit form.
    """
    tag = upstream.tag
    is_commit = len(tag) == 40 and all(c in "0123456789abcdef" for c in tag)
    if is_commit:
        return f"{upstream.repo}/archive/{tag}.zip"
    return f"{upstream.repo}/archive/refs/tags/{tag}.zip"


def install_one(upstream, into: Path) -> None:
    """Download, verify and unpack one archive, stripping its single top-level directory."""
    url = url_of(upstream)
    with urllib.request.urlopen(url, timeout=600) as response:
        data = response.read()

    digest = hashlib.sha256(data).hexdigest()
    if digest != upstream.pin_hash:
        sys.exit(
            f"{upstream.name}: sha256 {digest} does not match the pin {upstream.pin_hash}.\n"
            "Refusing to install it. Bump dependency.yml after vetting the new release."
        )

    target = into / SUBDIR[upstream.name]
    target.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        # A GitHub source archive has exactly one top-level directory, named for the tag; everything installs below it.
        roots = {PurePosixPath(m).parts[0] for m in archive.namelist() if m}
        if len(roots) != 1:
            sys.exit(f"{upstream.name}: expected one top-level directory in the archive, found {sorted(roots)}")
        root = roots.pop()

        for member in archive.infolist():
            path = PurePosixPath(member.filename)
            if path.is_absolute() or ".." in path.parts:
                sys.exit(f"refusing to extract {member.filename!r} (path traversal)")

            relative = PurePosixPath(*path.parts[1:]) if path.parts[0] == root else path
            if not relative.parts:
                continue

            out = target / Path(*relative.parts)
            if member.is_dir():
                out.mkdir(parents=True, exist_ok=True)
                continue

            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(archive.read(member))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true", help="re-download even when the pin already matches")
    args = parser.parse_args()

    upstreams = deps_manifest.load(DEST)
    pin = pin_over(upstreams)

    if not args.force and already_installed(pin):
        print("NRD already installed")
        return

    # Into a staging directory, moved into place only once every archive has arrived and verified.
    # A half-fetched .install/ carrying an old pin.txt would look complete to the CMake check.
    staging = DEST / ".install.staging"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)

    for index, upstream in enumerate(upstreams, start=1):
        print(f"  [{index}/{len(upstreams)}] {upstream.name} {upstream.version}")
        install_one(upstream, staging)

    (staging / "pin.txt").write_text(pin + "\n", encoding="utf-8")

    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    staging.rename(INSTALL)

    print(f"NRD installed into {INSTALL}")
    print("Configure again to pick it up: uv run dev.py configure")


if __name__ == "__main__":
    main()
