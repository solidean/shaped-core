#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# dependencies = ["pyyaml>=6"]
# ///
"""Download the pinned OpenXR loader into extern/openxr/.install.

The loader is what an OpenXR application links: it finds the active runtime, Meta Horizon Link or SteamVR on Windows,
and forwards every call to it.
Khronos ships it prebuilt in each release's NuGet package, which holds the headers and the x64 DLL and import library.

What is installed: include/openxr/*.h, bin/openxr_loader.dll, lib/openxr_loader.lib, and LICENSE, which the package
does not carry and which is taken from the same tag of the repository.

Pinning lives in dependency.yml next to this script: `tag` and `asset` name the package, and `pin_hash` (its SHA-256)
is the authority.
Re-running is idempotent; --force re-downloads anyway.
"""

import argparse
import hashlib
import io
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402


def download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "shaped-core-openxr-fetch"})
    with urllib.request.urlopen(request, timeout=600) as response:  # noqa: S310 (pinned GitHub URLs)
        return response.read()


def main() -> int:
    ap = argparse.ArgumentParser(description="Download the pinned OpenXR loader.")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    args = ap.parse_args()

    up = deps_manifest.one(DEST)
    if not up.is_available:
        print("openxr: the prebuilt loader is Windows-only — nothing to install on this host")
        return 0

    if not args.force and PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == up.pin_hash:
        print(f"openxr {up.version} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    print(f"downloading {up.name} {up.version} ...", flush=True)
    data = download(up.url)
    got = hashlib.sha256(data).hexdigest()
    if got != up.pin_hash:
        sys.exit(f"sha256 mismatch for {up.asset}: got {got}, expected {up.pin_hash}.\n"
                 "Update tag, asset, version and pin_hash together in dependency.yml.")
    license_text = download(f"{up.repo.replace('github.com', 'raw.githubusercontent.com')}/{up.tag}/LICENSE")
    if b"Apache License" not in license_text:
        sys.exit("the repository's LICENSE at the pinned tag is not the Apache license it was — re-read it before bumping")

    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        names = zf.namelist()
        plan = [("native/x64/release/bin/openxr_loader.dll", Path("bin/openxr_loader.dll")),
                ("native/x64/release/lib/openxr_loader.lib", Path("lib/openxr_loader.lib"))]
        plan += [(n, Path(n)) for n in names if n.startswith("include/openxr/") and n.endswith(".h")]
        missing = [member for member, _ in plan if member not in names]
        if missing or len(plan) < 4:
            sys.exit(f"{up.asset} is missing {', '.join(missing) or 'its headers'} — its layout changed; the installed copy is untouched")

        staging = INSTALL.with_name(".install.staging")
        if staging.exists():
            shutil.rmtree(staging)
        for member, dest in plan:
            (staging / dest).parent.mkdir(parents=True, exist_ok=True)
            with zf.open(member) as src, open(staging / dest, "wb") as out:
                shutil.copyfileobj(src, out)
    (staging / "LICENSE").write_bytes(license_text)

    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    staging.rename(INSTALL)
    PIN_FILE.write_text(up.pin_hash + "\n", encoding="utf-8")
    print(f"installed {up.name} {up.version} -> {INSTALL.as_posix()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
