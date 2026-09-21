#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6"]
# ///
"""Download the pinned Open Image Denoise release into extern/oidn/.install.

OIDN backs shaped-rendering's `sr::denoise_method::oidn`, the trained spatial denoise member.

Unlike the two NVIDIA SDKs this IS fetched for you: OIDN is Apache-2.0, so there is no license anyone has to accept,
and dev.py hydrates it on demand exactly as it does SDL3 and DXC.

**A CPU-ONLY SUBSET is installed**, not the whole archive.
The release ships device modules for CUDA, HIP and SYCL that together weigh 22 MB and that nothing can use yet: OIDN's
GPU devices share memory with ours through an OS handle, which needs exportable memory and shared fences sg does not
have. The CPU device needs neither — it reads and writes ordinary memory, which is what a download and an upload
already produce.

Members are selected BY BASENAME rather than by directory, because a Unix build puts its shared libraries under lib/
where Windows puts them under bin/, and only the Windows layout has actually been unpacked by anyone here.

Re-running is idempotent; pass --force to re-download anyway.
"""

import argparse
import hashlib
import io
import shutil
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

# This script lives in extern/oidn/ and installs alongside itself.
DEST = Path(__file__).resolve().parent
INSTALL = DEST / ".install"
PIN_FILE = INSTALL / "pin.txt"

# The pin lives in dependency.yml next to this script, so it is written once.
sys.path.insert(0, str(DEST.parent))
import deps_manifest  # noqa: E402

# What the CPU device actually needs, by basename stem.
# The core library carries the trained weights and is most of the download; the device module is the CPU backend
# itself; oneTBB is what it threads over.
KEEP_STEMS = (
    "OpenImageDenoise",
    "OpenImageDenoise_core",
    "OpenImageDenoise_device_cpu",
    "tbb12",
    "tbbbind",
    "tbbbind_2_0",
    "tbbbind_2_5",
)

# Library extensions across the three platforms, import libraries included.
LIB_SUFFIXES = (".dll", ".lib", ".so", ".dylib", ".pdb")

# License members, mapped onto the names `dependency.yml` declares so every platform writes the same files.
LICENSE_MEMBERS = {
    "LICENSE.txt": "LICENSE.txt",
    "third-party-programs.txt": "LICENSE-third-party-programs.txt",
    "third-party-programs-oneTBB.txt": "LICENSE-oneTBB.txt",
}


def wanted(relative: PurePosixPath) -> str | None:
    """Where `relative` installs to, or None to skip it.

    The returned path is relative to .install/, and deliberately flattens a platform's own layout onto one shape:
    headers under include/, everything loadable under bin/, import libraries under lib/.
    """
    name = relative.name

    if name in LICENSE_MEMBERS:
        return LICENSE_MEMBERS[name]

    if "include" in relative.parts:
        index = relative.parts.index("include")
        return str(PurePosixPath("include", *relative.parts[index + 1 :]))

    # A versioned Unix shared object is libfoo.so.2.5.1, so the suffix test has to look at the whole name.
    is_library = any(suffix in name for suffix in LIB_SUFFIXES)
    if not is_library:
        return None

    stem = name.split(".")[0]
    if stem.startswith("lib"):
        stem = stem[len("lib") :]
    if stem not in KEEP_STEMS:
        return None

    return str(PurePosixPath("lib" if name.endswith(".lib") else "bin", name))


def install_members(members: list[tuple[PurePosixPath, bytes]], into: Path) -> int:
    """Writes every member the plan keeps, and reports how many that was."""
    written = 0
    for relative, data in members:
        target = wanted(relative)
        if target is None:
            continue
        out = into / Path(target)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        written += 1
    return written


def read_archive(asset: str, data: bytes) -> list[tuple[PurePosixPath, bytes]]:
    """Every file in the archive, with its single top-level directory stripped."""
    out: list[tuple[PurePosixPath, bytes]] = []

    def add(name: str, payload: bytes) -> None:
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts:
            sys.exit(f"refusing to extract {name!r} (path traversal)")
        # Every release archive has one top-level directory named for the version.
        out.append((PurePosixPath(*path.parts[1:]) if len(path.parts) > 1 else path, payload))

    if asset.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for member in archive.infolist():
                if not member.is_dir():
                    add(member.filename, archive.read(member))
    else:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as archive:
            for member in archive.getmembers():
                if member.isfile():
                    handle = archive.extractfile(member)
                    if handle is not None:
                        add(member.name, handle.read())
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true", help="re-download even when the pin already matches")
    args = parser.parse_args()

    upstream = deps_manifest.one(DEST)
    if deps_manifest.host_os_key() in upstream.unavailable_on:
        sys.exit(f"OIDN publishes no release for {deps_manifest.host_os_key()}")

    if not args.force and PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == upstream.pin_hash:
        print("OIDN already installed")
        return

    print(f"  downloading {upstream.asset} ({upstream.version})")
    with urllib.request.urlopen(upstream.url, timeout=900) as response:
        data = response.read()

    digest = hashlib.sha256(data).hexdigest()
    if digest != upstream.pin_hash:
        sys.exit(
            f"OIDN: sha256 {digest} does not match the pin {upstream.pin_hash}.\n"
            "Refusing to install it. Bump dependency.yml after vetting the new release."
        )

    # Into a staging directory, moved into place only once everything has been written.
    # A half-installed .install/ carrying an old pin.txt would look complete to the CMake check.
    staging = DEST / ".install.staging"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)

    written = install_members(read_archive(upstream.asset, data), staging)

    # The core library alone is most of the download, so a plan that matched nothing would still produce a directory.
    # Refusing here is what turns a changed archive layout into a message rather than a link error much later.
    missing = [name for name in LICENSE_MEMBERS.values() if not (staging / name).is_file()]
    if written < 6 or missing:
        sys.exit(
            f"OIDN: the install plan kept {written} member(s) and is missing {missing or 'nothing'} — "
            f"the layout of {upstream.asset} is not what fetch-oidn.py expects."
        )

    (staging / "pin.txt").write_text(upstream.pin_hash + "\n", encoding="utf-8")

    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    staging.rename(INSTALL)

    total = sum(f.stat().st_size for f in INSTALL.rglob("*") if f.is_file())
    print(f"OIDN installed into {INSTALL} ({written} files, {total / 1e6:.1f} MB)")
    print("Configure again to pick it up: uv run dev.py configure")


if __name__ == "__main__":
    main()
