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
`--bump <tag>` rewrites the pin for a new release; vet its notes and licenses before committing.

Each platform's asset ships a different set of license members, so LICENSE_SOURCES maps them by name onto dependency.yml's `license_files`, which is what `dev.py deps licenses` collects.

Re-running is idempotent: a re-run whose .install/pin.txt already matches pin_hash is a no-op.
Pass --force to re-download anyway.
"""

import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import platform
import shutil
import sys
import urllib.request
import tarfile
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


class Archive:
    """A zip or a tar.gz behind one interface, since the Windows and Linux releases ship different formats."""

    def __init__(self, data: bytes, asset: str):
        self._is_tar = asset.endswith((".tar.gz", ".tgz"))
        if self._is_tar:
            self._tar = tarfile.open(fileobj=io.BytesIO(data), mode="r:gz")
        else:
            self._zip = zipfile.ZipFile(io.BytesIO(data))

    def names(self) -> list[str]:
        return self._tar.getnames() if self._is_tar else self._zip.namelist()

    def extract(self, member: str, dest: Path) -> None:
        dest.parent.mkdir(parents=True, exist_ok=True)
        if self._is_tar:
            src = self._tar.extractfile(member)
            if src is None:
                raise KeyError(member)
            with src, open(dest, "wb") as out:
                shutil.copyfileobj(src, out)
        else:
            with self._zip.open(member) as src, open(dest, "wb") as out:
                shutil.copyfileobj(src, out)

    def find(self, suffix: str) -> str | None:
        """The member whose path ends with `suffix`, since a tar prefixes every entry with `./`."""
        for name in self.names():
            if name == suffix or name.endswith("/" + suffix):
                return name
        return None


# Which release member each declared `license_files` destination is copied from, by the member's upper-cased stem.
# Matched by name, never by position: the Windows zip ships LICENSE-MS, LICENSE-MIT and LICENSE-LLVM, the Linux tarball
# only LICENSE-MS and LICENSE-LLVM, so a positional pairing put the MIT text under the LLVM name on one OS and not the other.
LICENSE_SOURCES = {
    "LICENSE.TXT": ("LICENSE-MS", "LICENSE"),
    "LICENSE-LLVM.TXT": ("LICENSE-LLVM",),
    "LICENSE-MIT.TXT": ("LICENSE-MIT",),
}


def license_members(archive: Archive) -> dict[str, str]:
    """The release's top-level license members, keyed by upper-cased stem.

    Anything under a subdirectory is skipped: those belong to bundled headers rather than to DXC itself.
    """
    out = {}
    for name in archive.names():
        path = Path(name)
        depth = len([p for p in path.parts if p not in (".", "")])
        if path.name.upper().startswith("LICENSE") and depth == 1:
            out[path.stem.upper()] = name
    return out


def install_plan(is_windows: bool, arch: str) -> list[tuple[str, Path]]:
    """Which member each installed file comes from, as a path suffix `Archive.find` resolves."""
    if is_windows:
        # The compiler DLL plus its dxil.dll signer, the import lib, and the two headers dxcapi.h needs
        # (d3d12shader.h sits next to it for dxcapi.h's own include, and backs the DXIL reflection path).
        return [
            (f"bin/{arch}/dxcompiler.dll", Path("bin/dxcompiler.dll")),
            (f"bin/{arch}/dxil.dll", Path("bin/dxil.dll")),
            (f"lib/{arch}/dxcompiler.lib", Path("lib/dxcompiler.lib")),
            ("inc/dxcapi.h", Path("include/dxc/dxcapi.h")),
            ("inc/d3d12shader.h", Path("include/dxc/d3d12shader.h")),
        ]
    # No import library, and no d3d12shader.h at all: the Linux release ships no DXIL reflection interfaces, which
    # is why the SPIR-V path reflects the emitted module instead of the container.
    # WinAdapter.h is what gives dxcapi.h its HRESULT / CComPtr / IID_PPV_ARGS off Windows, so it is not optional.
    return [
        ("lib/libdxcompiler.so", Path("lib/libdxcompiler.so")),
        ("include/dxc/dxcapi.h", Path("include/dxc/dxcapi.h")),
        ("include/dxc/WinAdapter.h", Path("include/dxc/WinAdapter.h")),
        ("include/dxc/dxcerrors.h", Path("include/dxc/dxcerrors.h")),
        ("include/dxc/Support/ErrorCodes.h", Path("include/dxc/Support/ErrorCodes.h")),
    ]


def missing_members(archive: Archive, plan: list[tuple[str, Path]]) -> list[str]:
    return [name for name, _ in plan if archive.find(name) is None]


# The two release assets a pin names, told apart from the PDB zip that ships beside them.
WINDOWS_ASSET = re.compile(r"^dxc_[\w.-]+\.zip$")
# Loose on the architecture suffix: upstream has spelled it both x86_64 and x86_x64.
LINUX_ASSET = re.compile(r"^linux_dxc_[\w.-]+\.tar\.gz$")


def github_json(url: str) -> dict:
    headers = {"User-Agent": "shaped-core-dxc-fetch", "Accept": "application/vnd.github+json"}
    token = os.environ.get("GITHUB_TOKEN")
    if not token:
        try:
            token = subprocess.run(["gh", "auth", "token"], capture_output=True, text=True, check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            token = ""
    if token:
        headers["Authorization"] = f"Bearer {token}"
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers)) as response:  # noqa: S310
        return json.load(response)


def bump(tag: str) -> int:
    """Point dependency.yml at release `tag`: resolve both assets, hash them, and rewrite the pin fields in place.

    The vetting stays a human's: read the release notes and both assets' license members before committing.
    """
    up = deps_manifest.one(DEST)
    repo = up.repo.removeprefix("https://github.com/").removesuffix(".git")
    release = github_json(f"https://api.github.com/repos/{repo}/releases/tags/{tag}")
    names = [a["name"] for a in release.get("assets", [])]

    picked = {}
    for key, pattern in (("windows", WINDOWS_ASSET), ("linux", LINUX_ASSET)):
        matches = [n for n in names if pattern.match(n)]
        if len(matches) != 1:
            sys.exit(f"{tag}: expected one {key} asset matching {pattern.pattern}, found {matches or 'none'} in {names}")
        picked[key] = matches[0]

    manifest = deps_manifest.manifest_path(DEST)
    text = manifest.read_text(encoding="utf-8")
    fields = {"version": tag.removeprefix("v"), "tag": tag}
    for key, asset in picked.items():
        url = f"{up.repo}/releases/download/{tag}/{asset}"
        print(f"downloading {asset} ...", flush=True)
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "shaped-core-dxc-fetch"})) as r:  # noqa: S310
            data = r.read()
        fields[f"asset_{key}"] = asset
        fields[f"pin_hash_{key}"] = hashlib.sha256(data).hexdigest()

        archive = Archive(data, asset)
        missing = missing_members(archive, install_plan(key == "windows", "x64" if key == "windows" else "x86_64"))
        if missing:
            sys.exit(f"{asset} is missing {', '.join(missing)}, which the install needs — its layout changed, so "
                     f"install_plan has to follow it first; dependency.yml is unchanged")

        # The license members are what a bump must re-read, so name them now rather than after the install.
        members = license_members(archive)
        mapped = {s for sources in LICENSE_SOURCES.values() for s in sources}
        print(f"  licenses: {', '.join(sorted(members)) or 'none'}")
        for stem in sorted(set(members) - mapped):
            print(f"  warning: {members[stem]} is new — read it, then add it to LICENSE_SOURCES and license_files",
                  file=sys.stderr)

    for key, value in fields.items():
        text, count = re.subn(rf"^(\s+{re.escape(key)}:\s*).*$", rf"\g<1>{value}", text, count=1, flags=re.M)
        if count != 1:
            sys.exit(f"{manifest}: no `{key}:` line to rewrite")
    manifest.write_text(text, encoding="utf-8")

    print(f"\n{manifest.relative_to(DEST.parent.parent).as_posix()} now pins {tag}.")
    print(f"  read the release notes: {release.get('html_url', '')}")
    print("  then: `uv run dev.py deps licenses`, `uv run dev.py check --fix`, and commit the manifest with the licenses")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Download the pinned DXC release binaries.")
    ap.add_argument("--force", action="store_true", help="re-download even if the install is current")
    ap.add_argument("--bump", metavar="TAG", help="rewrite dependency.yml to pin release TAG (e.g. v1.9.2607), then install it")
    args = ap.parse_args()

    if args.bump:
        bump(args.bump)
        args.force = True

    up = deps_manifest.one(DEST)

    # The host has no release at all — macOS today, which the manifest says outright via `unavailable_on`.
    # Asked rather than inferred from the platform, so the script and the pin cannot disagree about what exists.
    if not up.is_available:
        print(f"dxc: upstream publishes no {deps_manifest.host_os_key()} build — skipping (DXC stays unavailable)")
        return 0

    if not args.force and already_installed(up.pin_hash):
        print(f"dxc {up.tag} already installed at {INSTALL.as_posix()} — nothing to do")
        return 0

    # The Windows release lays its members out per architecture; the Linux one is flat and ships x86_64 only.
    is_windows = sys.platform == "win32"
    if not is_windows and host_arch() != "x64":
        # Upstream publishes no arm64 Linux binary, so there is nothing to install rather than something to fail on.
        # Installing the x86_64 one anyway is worse than skipping: CMake takes the presence of .install as "DXC is
        # available" and every dependent target then fails to link against a foreign-architecture .so.
        print(f"dxc: no {platform.machine()} linux binary published upstream — skipping (DXC stays unavailable)")
        return 0
    arch = host_arch() if is_windows else "x86_64"
    print(f"downloading {up.name} {up.tag} ({up.asset}, {arch}) ...", flush=True)
    request = urllib.request.Request(up.url, headers={"User-Agent": "shaped-core-dxc-fetch"})
    with urllib.request.urlopen(request) as response:  # noqa: S310 (pinned github release URL)
        data = response.read()

    got = hashlib.sha256(data).hexdigest()
    if got != up.pin_hash:
        sys.exit(f"sha256 mismatch for {up.asset}: got {got}, expected {up.pin_hash}.\n"
                 "Update tag/version/asset/pin_hash together in dependency.yml, after vetting the new release.")

    archive = Archive(data, up.asset)
    plan = install_plan(is_windows, arch)
    missing = missing_members(archive, plan)
    if missing:
        sys.exit(f"{up.asset} is missing {', '.join(missing)}, which this install needs — the installed copy is untouched")

    # Staged beside the live install and swapped in only once complete, so a failed extract never leaves DXC half-gone.
    staging = INSTALL.with_name(".install.staging")
    if staging.exists():
        shutil.rmtree(staging)
    for name, dest in plan:
        archive.extract(archive.find(name), staging / dest)

    # The licenses, so `dev.py deps licenses` has something to collect for a binary-only dependency.
    # A declared license a platform's asset lacks is a note, and one nobody declared is a warning to read and declare.
    members = license_members(archive)
    used = set()
    for declared in up.license_files:
        sources = LICENSE_SOURCES.get(Path(declared).name.upper())
        if sources is None:
            sys.exit(f"dependency.yml declares {declared}, which download-dxc.py's LICENSE_SOURCES does not map")
        stem = next((s for s in sources if s in members), None)
        if stem is None:
            # Not every platform's asset ships every license; `dev.py deps licenses` keeps the committed copy then.
            print(f"note: {up.asset} ships no {' / '.join(sources)} member for {declared}")
            continue
        used.add(stem)
        archive.extract(members[stem], staging / Path(declared).relative_to(".install"))
    for stem, member in members.items():
        if stem not in used:
            print(f"warning: {up.asset} ships {member}, which no license_files entry collects — read it and declare it",
                  file=sys.stderr)

    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    staging.rename(INSTALL)
    installed = ", ".join(dest.as_posix() for _, dest in plan)
    PIN_FILE.write_text(up.pin_hash + "\n", encoding="utf-8")

    print(f"\ninstalled {up.name} {up.tag} ({arch}) -> {INSTALL.as_posix()}")
    print(f"  {installed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
