#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Download NVIDIA's ORCA release of Amazon Lumberyard Bistro and install it into extern/bistro/.install.

The scene is what shaped-viewer's bistro example traces, and what the FBX importer's Bistro tests read when it is present.
It is fetched ON REQUEST, never by a configure: the archive is about 850 MB, and the example says how to fetch it when it is missing.

**The person running this accepts the scene's terms, or nothing is downloaded.**
They are Creative Commons Attribution 4.0, whose one condition is crediting the work wherever it is shown.
The script prints them and asks; --accept-license answers for a run with no one at the keyboard.

The whole archive is extracted, below its Bistro_v5_2/ folder, which is about 1.6 GB.
That is three FBX scenes, their 633 textures, Falcor's .pyscene files, the sky map, and the README, CHANGELOG and LICENSE that travel with them.

The archive is streamed to disk and hashed on the way.
`pin_hash` in dependency.yml is its SHA-256, and the download is rejected unless it matches.

Re-running is idempotent: a re-run whose .install/pin.txt already matches pin_hash is a no-op.
Pass --force to re-download anyway, and --keep-archive to keep the downloaded archive next to this script.
--accept-license accepts the terms without the prompt.
--archive PATH installs from an archive already on disk, such as one downloaded from NVIDIA's page by hand; it is verified the same way and left in place.
"""

import argparse
import hashlib
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

# This script lives in extern/bistro/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"
ARCHIVE = DEST / "Bistro_v5_2.zip.part"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Everything in the archive sits below this folder, which the install drops.
ARCHIVE_ROOT = "Bistro_v5_2/"

# What the install must contain for anything here to work; an archive missing one is refused before extracting.
REQUIRED = ["BistroExterior.fbx", "BistroInterior.fbx", "BistroInterior_Wine.fbx", "san_giuseppe_bridge_4k.hdr", "LICENSE.txt", "README.txt"]

CHUNK = 8 << 20

# What the archive's README.txt and LICENSE.txt say, shown before anything is downloaded; both land beside the scene.
TERMS = """Amazon Lumberyard Bistro, Open Research Content Archive (ORCA), Amazon Lumberyard, 2017.
  https://developer.nvidia.com/orca/amazon-lumberyard-bistro

It is licensed under the Creative Commons Attribution 4.0 International Public License:
  https://creativecommons.org/licenses/by/4.0/legalcode
Anything showing the scene credits it as above, which is what the README asks a publication to cite.
The sky map, san_giuseppe_bridge_4k.hdr, is from Poly Haven (then HDRI Haven) and is CC0.
The full text is installed as extern/bistro/.install/LICENSE.txt.
"""


def accept_terms(accepted_by_flag: bool) -> None:
    """Shows the terms and returns only once they are accepted; anything else ends the run before a byte is fetched."""
    print(TERMS, flush=True)
    if accepted_by_flag:
        print("terms accepted with --accept-license\n", flush=True)
        return
    if not sys.stdin.isatty():
        sys.exit("nothing was downloaded: these terms need accepting, and no one is at the keyboard to do it.\n"
                 "Read them, then re-run with --accept-license.")
    answer = input("Type 'yes' to accept these terms: ")
    if answer.strip().lower() != "yes":
        sys.exit("nothing was downloaded: the terms were not accepted.")


def already_installed(pin_hash: str) -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == pin_hash


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str) -> str:
    """Streams `url` into ARCHIVE and returns its SHA-256."""
    digest = hashlib.sha256()
    request = urllib.request.Request(url, headers={"User-Agent": "shaped-core-bistro-fetch"})
    with urllib.request.urlopen(request) as response, ARCHIVE.open("wb") as out:  # noqa: S310 (pinned by digest)
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        reported = 0
        while chunk := response.read(CHUNK):
            out.write(chunk)
            digest.update(chunk)
            done += len(chunk)
            if done - reported >= 128 << 20:
                reported = done
                of = f" of {total >> 20} MB" if total else ""
                print(f"  {done >> 20} MB{of}", flush=True)
    return digest.hexdigest()


def safe_target(name: str) -> Path:
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        sys.exit(f"refusing to extract {name!r} (path traversal)")
    return INSTALL.joinpath(*path.parts)


def extract(archive: zipfile.ZipFile) -> int:
    """Extracts every file below ARCHIVE_ROOT into INSTALL, returning how many it wrote."""
    names = [n for n in archive.namelist() if n.startswith(ARCHIVE_ROOT) and not n.endswith("/")]
    missing = [r for r in REQUIRED if ARCHIVE_ROOT + r not in names]
    if missing:
        sys.exit(f"the archive lacks {len(missing)} file(s) the install needs, the first {missing[0]!r}.")

    for name in names:
        target = safe_target(name[len(ARCHIVE_ROOT):])
        target.parent.mkdir(parents=True, exist_ok=True)
        with archive.open(name) as src, target.open("wb") as dst:
            shutil.copyfileobj(src, dst, CHUNK)
    return len(names)


def main() -> int:
    ap = argparse.ArgumentParser(description="Download NVIDIA's ORCA Amazon Lumberyard Bistro and install it.")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    ap.add_argument("--keep-archive", action="store_true", help="keep the downloaded archive next to this script")
    ap.add_argument("--archive", type=Path, help="install from this archive instead of downloading it")
    ap.add_argument("--accept-license", action="store_true", help="accept the scene's CC BY 4.0 terms without the prompt")
    args = ap.parse_args()

    up = deps_manifest.one(DEST)

    if not args.force and already_installed(up.pin_hash):
        print(f"{up.name} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    accept_terms(args.accept_license)

    archive_path = args.archive or ARCHIVE
    if args.archive:
        print(f"verifying {args.archive} ...", flush=True)
        got = hash_file(args.archive)
    else:
        print(f"downloading {up.name} ({up.asset}, about 850 MB) ...", flush=True)
        got = download(up.url)
    if got != up.pin_hash:
        if not args.archive:
            ARCHIVE.unlink(missing_ok=True)
        sys.exit(f"sha256 mismatch for {up.asset}: got {got}, expected {up.pin_hash}.\n"
                 "NVIDIA may have republished the scene; vet the new archive, then update pin_hash and this script's names together.")

    # Fresh install (drop any prior version).
    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    INSTALL.mkdir(parents=True)

    print(f"extracting to {INSTALL.as_posix()} ...", flush=True)
    with zipfile.ZipFile(archive_path) as archive:
        written = extract(archive)

    if not args.archive and not args.keep_archive:
        ARCHIVE.unlink()

    # Written last, so an interrupted run leaves an install that fails already_installed().
    PIN_FILE.write_text(up.pin_hash + "\n", encoding="utf-8")

    print(f"\ninstalled {up.name} ({written} files) -> {INSTALL.as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
