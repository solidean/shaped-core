#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Download the pinned DirectX Shader Compiler release binaries into extern/dxc/.install.

DXC is neither vendored as source nor built from source — its from-source build is
LLVM-scale (~20 min), far too slow to run in CI. Instead we download the official
prebuilt Windows release for the host architecture, verify its SHA-256, and extract the
dxcompiler DLL + import lib + headers (plus the dxil.dll signer, so emitted DXIL is
signed) into a gitignored extern/dxc/.install/. The download is small and fast, so
dev.py runs it on demand per configure (see tools/dev/lib/pipeline/prereqs.py).

Pinning: PIN_TAG is the human-readable release; PIN_HASH (the asset's SHA-256) is the
authority — the download is rejected unless it matches. Bump PIN_TAG/ASSET/PIN_HASH
together after vetting a new release.

Re-running is idempotent: a re-run whose .install/pin.txt already matches PIN_HASH is a
no-op. Pass --force to re-download anyway.
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

# Pinned upstream release. https://github.com/microsoft/DirectXShaderCompiler/releases
REPO_URL = "https://github.com/microsoft/DirectXShaderCompiler"
PIN_TAG = "v1.9.2602.24"
ASSET = "dxc_2026_05_27.zip"
PIN_HASH = "cf658aacf070d3045e31b8f1f8a696c2945f37c1095019481ef7c513368db3b4"
URL = f"{REPO_URL}/releases/download/{PIN_TAG}/{ASSET}"

# This script lives in extern/dxc/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

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


def already_installed() -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == PIN_HASH


def main() -> int:
    ap = argparse.ArgumentParser(description="Download the pinned DXC release binaries.")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    args = ap.parse_args()

    if not args.force and already_installed():
        print(f"dxc {PIN_TAG} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    if sys.platform != "win32":
        sys.exit("download-dxc.py currently supports Windows only (uses the Windows release + d3d12shader.h reflection)")

    arch = host_arch()
    print(f"downloading DXC {PIN_TAG} ({ASSET}, {arch}) ...", flush=True)
    request = urllib.request.Request(URL, headers={"User-Agent": "shaped-core-dxc-fetch"})
    with urllib.request.urlopen(request) as response:  # noqa: S310 (pinned github release URL)
        data = response.read()

    got = hashlib.sha256(data).hexdigest()
    if got != PIN_HASH:
        sys.exit(f"sha256 mismatch for {ASSET}: got {got}, expected {PIN_HASH}.\n"
                 "Update PIN_TAG/ASSET/PIN_HASH together after vetting the new release.")

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

    PIN_FILE.write_text(PIN_HASH + "\n", encoding="utf-8")

    print(f"\ninstalled DXC {PIN_TAG} ({arch}) -> {INSTALL.as_posix()}")
    print("  bin/dxcompiler.dll, bin/dxil.dll, lib/dxcompiler.lib, include/dxc/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
