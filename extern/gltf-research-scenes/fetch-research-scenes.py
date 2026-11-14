#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# dependencies = ["pyyaml>=6"]
# ///
"""Download the glTF Research Scenes release and install its CC0 and CC-BY-3.0 scenes into extern/gltf-research-scenes/.install.

The scenes are what shaped-viewer's research-scenes example shows.
They are fetched ON REQUEST, never by a configure: the archive is about 720 MiB, and the example says how to fetch it when it is missing.

**The person running this accepts the scenes' terms, or nothing is downloaded.**
Thirteen scenes are CC0, and ten are Creative Commons Attribution 3.0, whose one condition is crediting each author wherever the scene is shown.
The script prints them and asks; --accept-license answers for a run with no one at the keyboard.
The four scenes whose licenses are mixed stay in the archive.

Each kept scene lands as .install/<scene>/, with its GLB files, its reference render, its conversion report and its license.
CREDITS.txt beside them gathers every kept scene's license text, which is what the example reads to credit a scene.

The archive is streamed to disk and hashed on the way.
`pin_hash` in dependency.yml is its SHA-256, and the download is rejected unless it matches.

Re-running is idempotent: a re-run whose .install/pin.txt already matches pin_hash is a no-op.
Pass --force to re-download anyway, and --keep-archive to keep the downloaded archive next to this script.
--accept-license accepts the terms without the prompt.
--archive PATH installs from an archive already on disk; it is verified the same way and left in place.
"""

import argparse
import hashlib
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

# This script lives in extern/gltf-research-scenes/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
STAGING = DEST / ".install.staging"
PIN_FILE = INSTALL / "pin.txt"
CREDITS = "CREDITS.txt"
ARCHIVE = DEST / "gltf_research_scenes_glb.zip.part"
# Where --keep-archive leaves a verified download, under the name --archive is then given.
KEPT_ARCHIVE = DEST / "gltf_research_scenes_glb.zip"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# Every scene sits in its own folder below this one, which the install drops.
ARCHIVE_ROOT = "gltf_research_scenes/"

# The scenes this script may install, by the license the release's README gives each.
CC0 = ["bathroom", "bmw-m6", "classroom2", "cornell-box", "cornell-caustic", "glass-of-water", "house", "lamp",
       "pontiac-gto", "spaceship", "veach-ajar", "veach-bidir", "veach-mis"]
CC_BY_3 = ["breakfast-room", "bathroom2", "classroom", "coffee-maker", "dragon", "kitchen", "living-room",
           "living-room-2", "modern-hall", "staircase"]
SCENES = CC0 + CC_BY_3

CHUNK = 8 << 20

TERMS = f"""glTF Research Scenes v1.1.0, by Erfan Mohammadi, https://github.com/ErfanMo77/gltf-research-scenes
Most are Benedikt Bitterli's rendering resources, https://benedikt-bitterli.me/resources/, converted to glTF.

These {len(CC0)} scenes are CC0 (public domain), and may be used without attribution:
  {", ".join(CC0)}

These {len(CC_BY_3)} scenes are Creative Commons Attribution 3.0, https://creativecommons.org/licenses/by/3.0/:
  {", ".join(CC_BY_3)}
Anything showing one credits its original author and links the license; each scene's license file names the author.

The four scenes whose licenses are mixed (material-testball, teapot, teapot-full, vintage-car) are not installed.
Every installed scene keeps its own license file, and .install/{CREDITS} gathers them all.
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
    request = urllib.request.Request(url, headers={"User-Agent": "shaped-core-research-scenes-fetch"})
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


def safe_target(name: str, root: Path) -> Path:
    # A backslash is a separator on Windows, and a colon names a drive, so neither may hide inside one POSIX part.
    path = PurePosixPath(name.replace("\\", "/"))
    if ":" in name or path.is_absolute() or ".." in path.parts:
        sys.exit(f"refusing to extract {name!r} (path traversal)")
    target = root.joinpath(*path.parts)
    if not target.resolve().is_relative_to(root.resolve()):
        sys.exit(f"refusing to extract {name!r} (it resolves outside {root.as_posix()})")
    return target


def scene_of(name: str) -> str:
    """The scene folder an archive member sits in, or '' for one outside every scene."""
    rest = name[len(ARCHIVE_ROOT):] if name.startswith(ARCHIVE_ROOT) else ""
    return rest.split("/", 1)[0] if "/" in rest else ""


def extract(archive: zipfile.ZipFile, root: Path) -> tuple[int, list[str]]:
    """Extracts every file of the kept scenes into root, returning how many it wrote and each scene's license text."""
    names = [n for n in archive.namelist() if not n.endswith("/") and scene_of(n) in SCENES]
    missing = [s for s in SCENES if not any(scene_of(n) == s for n in names)]
    if missing:
        sys.exit(f"the archive lacks {len(missing)} scene(s) the install expects, the first {missing[0]!r}.")

    licenses = []
    for name in names:
        target = safe_target(name[len(ARCHIVE_ROOT):], root)
        target.parent.mkdir(parents=True, exist_ok=True)
        with archive.open(name) as src, target.open("wb") as dst:
            shutil.copyfileobj(src, dst, CHUNK)
        if PurePosixPath(name).name.upper().endswith("LICENSE.TXT"):
            text = target.read_text(encoding="utf-8", errors="replace").strip()
            licenses.append(f"== {scene_of(name)} ({'CC0-1.0' if scene_of(name) in CC0 else 'CC-BY-3.0'})\n\n{text}\n")
    unlicensed = [s for s in SCENES if not any(l.startswith(f"== {s} ") for l in licenses)]
    if unlicensed:
        sys.exit(f"{len(unlicensed)} scene(s) carry no license file, the first {unlicensed[0]!r}; refusing to install them unlicensed.")
    return len(names), sorted(licenses)


def main() -> int:
    ap = argparse.ArgumentParser(description="Download the glTF Research Scenes and install their CC0 and CC-BY-3.0 scenes.")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    ap.add_argument("--keep-archive", action="store_true", help="keep the downloaded archive next to this script")
    ap.add_argument("--archive", type=Path, help="install from this archive instead of downloading it")
    ap.add_argument("--accept-license", action="store_true", help="accept the scenes' CC0 and CC-BY-3.0 terms without the prompt")
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
        print(f"downloading {up.name} ({up.asset}, about 720 MiB) ...", flush=True)
        got = download(up.url)
    if got != up.pin_hash:
        if not args.archive:
            ARCHIVE.unlink(missing_ok=True)
        sys.exit(f"sha256 mismatch for {up.asset}: got {got}, expected {up.pin_hash}.\n"
                 "The release may have been republished; vet the new archive, then update pin_hash and this script's scene lists together.")

    # Into a staging directory, moved into place only once every file has arrived, so an interrupted run keeps the old install.
    if STAGING.exists():
        shutil.rmtree(STAGING)
    STAGING.mkdir(parents=True)

    print(f"extracting {len(SCENES)} scenes to {STAGING.as_posix()} ...", flush=True)
    with zipfile.ZipFile(archive_path) as archive:
        written, licenses = extract(archive, STAGING)
    (STAGING / CREDITS).write_text(
        "The scenes installed here, and the license each carries.\n"
        "A CC-BY-3.0 scene credits its author, as its text below names them, wherever it is shown.\n\n" + "\n".join(licenses),
        encoding="utf-8")
    (STAGING / PIN_FILE.name).write_text(up.pin_hash + "\n", encoding="utf-8")

    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    STAGING.rename(INSTALL)

    if not args.archive:
        if args.keep_archive:
            ARCHIVE.replace(KEPT_ARCHIVE)
            print(f"kept the archive as {KEPT_ARCHIVE.as_posix()}")
        else:
            ARCHIVE.unlink()

    print(f"\ninstalled {len(SCENES)} scenes ({written} files) -> {INSTALL.as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
