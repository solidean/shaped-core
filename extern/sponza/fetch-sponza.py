#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Download Intel's Sponza base scene and install its glTF into extern/sponza/.install.

The scene is what shaped-viewer's sponza example traces, and nothing else uses it.
It is fetched ON REQUEST, never by a configure: the archive is about 4 GB, and the example says how to fetch it when it is missing.

**The person running this accepts Intel's terms, or nothing is downloaded.**
They are not a plain open license: the credits file adds a use restriction above its CC BY 4.0 text, so accepting them is a decision a person makes.
The script prints them and asks; --accept-license answers for a run with no one at the keyboard.

Intel ships one archive holding the scene in four formats, which share one texture folder.
Only the glTF document, its buffer and the textures it names are extracted, together with the credits and license notes.
That is about 2 GB of the archive's 4.

The archive is streamed to disk and hashed on the way, since it does not fit comfortably in memory.
`pin_hash` in dependency.yml is its SHA-256, and the download is rejected unless it matches.

Re-running is idempotent: a re-run whose .install/pin.txt already matches pin_hash is a no-op.
Pass --force to re-download anyway, and --keep-archive to keep the downloaded archive next to this script.
--accept-license accepts the terms without the prompt.
--archive PATH installs from an archive already on disk, such as one downloaded from Intel's page by hand; it is verified the same way and left in place.
"""

import argparse
import hashlib
import json
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

# This script lives in extern/sponza/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"
ARCHIVE = DEST / "sponza-base.zip.part"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Everything in the archive sits below this folder, which the install drops.
ARCHIVE_ROOT = "main_sponza/"

# The glTF document, relative to ARCHIVE_ROOT; its own `buffers` and `images` say what else is extracted.
GLTF = "NewSponza_Main_glTF_003.gltf"

# The credits and the license text, which CC BY 4.0 requires to travel with the scene.
CREDITS = "credits_license.txt"

CHUNK = 8 << 20

# What the archive's credits_license.txt says, shown before anything is downloaded; the full text lands beside the scene.
TERMS = """Intel Sponza (2022), commissioned by Frank Meinl, sponsored by Anton Kaplanyan; Intel GPU Research Samples.
  https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html

Its credits file states, above the license text:
  "For personal use and educational use. Limited commercial use for marketing and print purposes."
The license text that follows is the Creative Commons Attribution 4.0 International Public License:
  https://creativecommons.org/licenses/by/4.0/legalcode
A publication using the scene is asked to cite it as Meinl and Kaplanyan, Intel Sample Library, 2022.
The full text is installed as extern/sponza/.install/credits_license.txt.
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
    request = urllib.request.Request(url, headers={"User-Agent": "shaped-core-sponza-fetch"})
    with urllib.request.urlopen(request) as response, ARCHIVE.open("wb") as out:  # noqa: S310 (pinned by digest)
        total = int(response.headers.get("Content-Length") or 0)
        done = 0
        reported = 0
        while chunk := response.read(CHUNK):
            out.write(chunk)
            digest.update(chunk)
            done += len(chunk)
            if done - reported >= 256 << 20:
                reported = done
                of = f" of {total >> 20} MB" if total else ""
                print(f"  {done >> 20} MB{of}", flush=True)
    return digest.hexdigest()


def safe_target(name: str) -> Path:
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        sys.exit(f"refusing to extract {name!r} (path traversal)")
    return INSTALL.joinpath(*path.parts)


def wanted_members(archive: zipfile.ZipFile) -> list[str]:
    """The archive paths the install needs: the document, every uri it references, and the credits."""
    document = json.loads(archive.read(ARCHIVE_ROOT + GLTF))
    uris = [b["uri"] for b in document.get("buffers", []) if "uri" in b]
    uris += [i["uri"] for i in document.get("images", []) if "uri" in i]
    return [ARCHIVE_ROOT + name for name in [GLTF, CREDITS, *uris]]


def extract(archive: zipfile.ZipFile) -> int:
    """Extracts what `wanted_members` names into INSTALL, below ARCHIVE_ROOT, returning how many files it wrote."""
    names = set(archive.namelist())
    wanted = wanted_members(archive)
    missing = [name for name in wanted if name not in names]
    if missing:
        sys.exit(f"the archive lacks {len(missing)} file(s) its glTF references, the first {missing[0]!r}.")

    for name in wanted:
        target = safe_target(name[len(ARCHIVE_ROOT):])
        target.parent.mkdir(parents=True, exist_ok=True)
        with archive.open(name) as src, target.open("wb") as dst:
            shutil.copyfileobj(src, dst, CHUNK)
    return len(wanted)


def main() -> int:
    ap = argparse.ArgumentParser(description="Download Intel's Sponza base scene and install its glTF.")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    ap.add_argument("--keep-archive", action="store_true", help="keep the downloaded archive next to this script")
    ap.add_argument("--archive", type=Path, help="install from this archive instead of downloading it")
    ap.add_argument("--accept-license", action="store_true", help="accept Intel's terms for the scene without the prompt")
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
        print(f"downloading {up.name} ({up.asset}, about 4 GB) ...", flush=True)
        got = download(up.url)
    if got != up.pin_hash:
        if not args.archive:
            ARCHIVE.unlink(missing_ok=True)
        sys.exit(f"sha256 mismatch for {up.asset}: got {got}, expected {up.pin_hash}.\n"
                 "Intel may have republished the scene; vet the new archive, then update pin_hash and this script's names together.")

    # Fresh install (drop any prior version).
    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    INSTALL.mkdir(parents=True)

    print(f"extracting the glTF to {INSTALL.as_posix()} ...", flush=True)
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
