#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Download the pinned Microsoft.Direct3D.WARP package into extern/warp/.install.

WARP is the software D3D12 adapter the dx12 tests run on where there is no GPU.
Windows ships one of its own, and that one traces some scenes wrong; docs/bugs-external/warp-raytracing-wrong-capture says which.
The NuGet package is a newer WARP, and a d3d10warp.dll beside an executable is loaded instead of the system one.
So this installs the host architecture's DLL and its license, and shaped-graphics stages the DLL beside every WARP test binary.

Fetched ON REQUEST, never by a configure: the package's terms are testing-only, forbid redistribution and carry their own
data-collection and arbitration clauses, which are a person's or a CI job's to accept rather than a build's.
`uv run extern/warp/fetch-warp.py` runs it, and nothing in dev.py does.

Pinning lives in dependency.yml next to this script: `version` names the package, and `pin_hash` (its SHA-256) is the authority.
Re-running is idempotent; --force re-downloads anyway.
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

DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# platform.machine() -> the package's per-arch folder.
ARCH_MAP = {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}


def main() -> int:
    ap = argparse.ArgumentParser(description="Download the pinned WARP package.")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    args = ap.parse_args()

    up = deps_manifest.one(DEST)
    if not up.is_available:
        print("warp: a Windows-only adapter — nothing to install on this host")
        return 0

    if not args.force and PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == up.pin_hash:
        print(f"warp {up.version} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    arch = ARCH_MAP.get(platform.machine().lower())
    if arch is None:
        sys.exit(f"the WARP package has no build for {platform.machine()!r}")

    print(f"downloading {up.name} {up.version} ({arch}) ...", flush=True)
    request = urllib.request.Request(up.url, headers={"User-Agent": "shaped-core-warp-fetch"})
    with urllib.request.urlopen(request, timeout=300) as response:  # noqa: S310 (pinned nuget.org package URL)
        data = response.read()

    got = hashlib.sha256(data).hexdigest()
    if got != up.pin_hash:
        sys.exit(f"sha256 mismatch for {up.asset}: got {got}, expected {up.pin_hash}.\n"
                 "Update version and pin_hash together in dependency.yml, after reading the new package's terms.")

    # The package is a zip; the DLL sits under build/native/bin/<arch>/, and LICENSE.TXT at its root.
    plan = [(f"build/native/bin/{arch}/d3d10warp.dll", Path("bin/d3d10warp.dll")), ("LICENSE.TXT", Path("LICENSE.txt"))]
    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        names = set(zf.namelist())
        missing = [member for member, _ in plan if member not in names]
        if missing:
            sys.exit(f"{up.asset} is missing {', '.join(missing)} — its layout changed; the installed copy is untouched")

        # Staged beside the live install and swapped in only once complete, so a failed extract never leaves it half-gone.
        staging = INSTALL.with_name(".install.staging")
        if staging.exists():
            shutil.rmtree(staging)
        for member, dest in plan:
            (staging / dest).parent.mkdir(parents=True, exist_ok=True)
            with zf.open(member) as src, open(staging / dest, "wb") as out:
                shutil.copyfileobj(src, out)

    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    staging.rename(INSTALL)
    PIN_FILE.write_text(up.pin_hash + "\n", encoding="utf-8")

    print(f"\ninstalled {up.name} {up.version} ({arch}) -> {INSTALL.as_posix()}")
    print("  reconfigure (any dev.py build does) so the WARP test binaries stage it beside themselves")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
