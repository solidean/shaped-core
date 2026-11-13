#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Download the pinned D3D12 Agility SDK (Microsoft.Direct3D.D3D12) into extern/agility/.install.

The Agility SDK is the D3D12 runtime newer than the one Windows carries.
Shader Model 6.9, DXR 1.2 and shader execution reordering arrive only through it.
With it installed, shaped-graphics' dx12 backend creates its devices through the SDK's D3D12Core.dll; without it, through
the system runtime, where those features report unsupported.

**Nothing runs this for you, and it refuses to run without `--accept-license`.**
The package's terms are Microsoft's DirectX license, with data-collection, indemnification and distribution clauses.
Accepting them is a person's or a CI job's decision, never a build's: read .install/LICENSE.txt's source, the package's
LICENSE.txt, before passing the flag.

What is installed: the headers, the host architecture's D3D12Core.dll and d3d12SDKLayers.dll, and both license texts.

Pinning lives in dependency.yml next to this script: `version` names the package, and `pin_hash` (its SHA-256) is the
authority.
Re-running is idempotent; --force re-downloads anyway.

`--preview` installs extern/agility-preview's pin into extern/agility-preview/.install instead: the pre-release runtime
with clustered geometry and partitioned TLAS, under narrower terms, read only by a build configured with
SC_AGILITY_PREVIEW.
Its dependency.yml says why it is a development dependency only.
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

HERE = Path(__file__).resolve().parent

sys.path.insert(0, str(HERE.parent))
import deps_manifest  # noqa: E402

# platform.machine() -> the package's per-arch folder.
ARCH_MAP = {"amd64": "x64", "x86_64": "x64", "arm64": "arm64", "aarch64": "arm64"}

# Runtime DLLs a package may carry beyond the two every one has: the preview's state objects compile through this one.
OPTIONAL_DLLS = ("D3D12StateObjectCompiler.dll",)


def main() -> int:
    ap = argparse.ArgumentParser(description="Download the pinned D3D12 Agility SDK.")
    ap.add_argument("--accept-license", action="store_true",
                    help="accept Microsoft's DirectX license terms of the pinned package")
    ap.add_argument("--preview", action="store_true",
                    help="install extern/agility-preview's pre-release pin instead of the retail one")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    args = ap.parse_args()

    dest = HERE.parent / "agility-preview" if args.preview else HERE
    install = dest / ".install"
    pin_file = install / "pin.txt"
    command = "uv run extern/agility/fetch-agility.py" + (" --preview" if args.preview else "")

    up = deps_manifest.one(dest)
    if not up.is_available:
        print("agility: the D3D12 runtime is Windows-only — nothing to install on this host")
        return 0

    if not args.accept_license:
        print(f"The {up.name} {up.version} is licensed under Microsoft's DirectX terms, which nothing accepts for you.\n"
              f"Read them in the package ({up.homepage}, its LICENSE.txt), then run:\n"
              f"    {command} --accept-license")
        return 1

    if not args.force and pin_file.is_file() and pin_file.read_text(encoding="utf-8").strip() == up.pin_hash:
        print(f"agility {up.version} already installed at {install.as_posix()} — nothing to do")
        return 0

    arch = ARCH_MAP.get(platform.machine().lower())
    if arch is None:
        sys.exit(f"the Agility SDK has no build for {platform.machine()!r}")

    print(f"downloading {up.name} {up.version} ({arch}) ...", flush=True)
    request = urllib.request.Request(up.url, headers={"User-Agent": "shaped-core-agility-fetch"})
    with urllib.request.urlopen(request, timeout=600) as response:  # noqa: S310 (pinned nuget.org package URL)
        data = response.read()

    got = hashlib.sha256(data).hexdigest()
    if got != up.pin_hash:
        sys.exit(f"sha256 mismatch for {up.asset}: got {got}, expected {up.pin_hash}.\n"
                 "Update version and pin_hash together in dependency.yml, after reading the new package's terms.")

    with zipfile.ZipFile(io.BytesIO(data)) as zf:
        names = zf.namelist()
        # The runtime under build/native/bin/<arch>/, the headers under build/native/include/, the licenses at the root.
        plan = [(f"build/native/bin/{arch}/{dll}", Path("bin") / dll) for dll in ("D3D12Core.dll", "d3d12SDKLayers.dll")]
        plan += [(f"build/native/bin/{arch}/{dll}", Path("bin") / dll) for dll in OPTIONAL_DLLS
                 if f"build/native/bin/{arch}/{dll}" in names]
        plan += [("LICENSE.txt", Path("LICENSE.txt")), ("LICENSE-CODE.txt", Path("LICENSE-CODE.txt"))]
        prefix = "build/native/include/"
        plan += [(n, Path("include") / n[len(prefix):]) for n in names if n.startswith(prefix) and not n.endswith("/")]
        missing = [member for member, _ in plan if member not in names]
        if missing:
            sys.exit(f"{up.asset} is missing {', '.join(missing)} — its layout changed; the installed copy is untouched")

        # Staged beside the live install and swapped in only once complete, so a failed extract never leaves it half-gone.
        staging = install.with_name(".install.staging")
        if staging.exists():
            shutil.rmtree(staging)
        for member, target in plan:
            (staging / target).parent.mkdir(parents=True, exist_ok=True)
            with zf.open(member) as src, open(staging / target, "wb") as out:
                shutil.copyfileobj(src, out)

    if install.exists():
        shutil.rmtree(install)
    staging.rename(install)
    pin_file.write_text(up.pin_hash + "\n", encoding="utf-8")

    print(f"\ninstalled {up.name} {up.version} ({arch}) -> {install.as_posix()}")
    if args.preview:
        print("  only a build configured with SC_AGILITY_PREVIEW reads it: `uv run dev.py build --agility-preview`")
    else:
        print("  reconfigure (any dev.py build does) so the dx12 backend creates its devices through it")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
