#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Download the pinned DirectX Shader Compiler release binaries into extern/dxc/.install.

DXC is neither vendored as source nor built from source — its from-source build is
LLVM-scale (~20 min), far too slow to run in CI. Instead we download the official
prebuilt Windows release for the host architecture, verify its SHA-256, and extract the
dxcompiler DLL + import lib + headers (plus the dxil.dll signer, so emitted DXIL is
signed) into a gitignored extern/dxc/.install/. The download is small and fast, so
dev.py runs it on demand per configure (see tools/dev/lib/pipeline/prereqs.py).

Pinning lives in dependency.yml next to this script: `tag` is the human-readable release, and `pin_hash` (the asset's SHA-256) is the authority,
so the download is rejected unless it matches.
Bump tag, version, asset and pin_hash together after vetting a new release.

Upstream names the license member inconsistently across releases, so it is found by pattern rather than by a fixed path.
Its destination is dependency.yml's `license_files` entry, which is what `dev.py deps licenses` collects.

Re-running is idempotent: a re-run whose .install/pin.txt already matches pin_hash is a no-op.
Pass --force to re-download anyway.
"""

import argparse
import hashlib
import io
import platform
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

# This script lives in extern/dxc/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# platform.machine() -> the release's per-arch subdirectory.
ARCH_MAP = {
    "amd64": "x64",
    "x86_64": "x64",
    "x64": "x64",
    "arm64": "arm64",
    "aarch64": "arm64",
    "x86": "x86",
    "i386": "x86",
    "i686": "x86",
}


def host_arch() -> str:
    arch = ARCH_MAP.get(platform.machine().lower())
    if arch is None:
        sys.exit(f"unsupported host architecture {platform.machine()!r} (need x64 / arm64 / x86)")
    return arch


def already_installed(pin_hash: str) -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == pin_hash


def license_member(archive: zipfile.ZipFile) -> str | None:
    """The release's license member, whose exact name has moved between releases (LICENSE.txt, LICENSE-MIT, ...)."""
    names = [n for n in archive.namelist() if Path(n).name.upper().startswith("LICENSE")]
    return min(names, key=len) if names else None


def main() -> int:
    ap = argparse.ArgumentParser(description="Download the pinned DXC release binaries.")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    args = ap.parse_args()

    up = deps_manifest.one(DEST)

    if not args.force and already_installed(up.pin_hash):
        print(f"dxc {up.tag} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    if sys.platform != "win32":
        sys.exit("download-dxc.py currently supports Windows only (uses the Windows release + d3d12shader.h reflection)")

    arch = host_arch()
    print(f"downloading {up.name} {up.tag} ({up.asset}, {arch}) ...", flush=True)
    request = urllib.request.Request(up.url, headers={"User-Agent": "shaped-core-dxc-fetch"})
    with urllib.request.urlopen(request) as response:  # noqa: S310 (pinned github release URL)
        data = response.read()

    got = hashlib.sha256(data).hexdigest()
    if got != up.pin_hash:
        sys.exit(f"sha256 mismatch for {up.asset}: got {got}, expected {up.pin_hash}.\n"
                 "Update tag/version/asset/pin_hash together in dependency.yml, after vetting the new release.")

    archive = zipfile.ZipFile(io.BytesIO(data))

    # Fresh install (drop any prior arch/version).
    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    (INSTALL / "bin").mkdir(parents=True)
    (INSTALL / "lib").mkdir()
    (INSTALL / "include" / "dxc").mkdir(parents=True)

    def extract(member: str, dest: Path) -> None:
        with archive.open(member) as src, open(dest, "wb") as out:
            shutil.copyfileobj(src, out)

    # The minimal set: the compiler DLL + its dxil.dll signer, the import lib, and the two headers
    # dxcapi.h needs (d3d12shader.h sits next to it for dxcapi.h's own include).
    extract(f"bin/{arch}/dxcompiler.dll", INSTALL / "bin" / "dxcompiler.dll")
    extract(f"bin/{arch}/dxil.dll", INSTALL / "bin" / "dxil.dll")
    extract(f"lib/{arch}/dxcompiler.lib", INSTALL / "lib" / "dxcompiler.lib")
    extract("inc/dxcapi.h", INSTALL / "include" / "dxc" / "dxcapi.h")
    extract("inc/d3d12shader.h", INSTALL / "include" / "dxc" / "d3d12shader.h")

    # The license, so `dev.py deps licenses` has something to collect for a binary-only dependency.
    # A release that ships none is not worth failing a build over, so warn and carry on.
    member = license_member(archive)
    if member is None:
        print(f"warning: {up.asset} ships no LICENSE member — docs/licenses/ will keep its committed copy", file=sys.stderr)
    else:
        license_dest = INSTALL.parent / up.license_files[0]
        license_dest.parent.mkdir(parents=True, exist_ok=True)
        extract(member, license_dest)

    PIN_FILE.write_text(up.pin_hash + "\n", encoding="utf-8")

    print(f"\ninstalled {up.name} {up.tag} ({arch}) -> {INSTALL.as_posix()}")
    print("  bin/dxcompiler.dll, bin/dxil.dll, lib/dxcompiler.lib, include/dxc/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
