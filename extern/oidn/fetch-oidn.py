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
where Windows puts them under bin/, and the install flattens both onto one shape.

SYMLINKS ARE PART OF THE PAYLOAD on the two Unix platforms.
Each library arrives as one versioned real file plus the unversioned and soname links onto it, and those links are the
names extern/oidn/CMakeLists.txt links by and the dynamic loader resolves through.
An install that keeps only regular files produces a directory that looks complete and builds nothing.

NOT EVERY PLATFORM HAS A RELEASE.
Upstream publishes x64 Windows, x86_64 Linux and arm64 macOS.
The other machines those systems run on are declared in `unavailable_on` and skipped here rather than handed an archive built for a different instruction set.

Re-running is idempotent; pass --force to re-download anyway.
"""

import argparse
from dataclasses import dataclass
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
# oneTBB is spelled `tbb12` on Windows and `tbb` on Unix, so both spellings must be kept — each matches only its own platform.
# The install is checked below for having landed one of them.
TBB_STEMS = ("tbb", "tbb12")
KEEP_STEMS = TBB_STEMS + (
    "OpenImageDenoise",
    "OpenImageDenoise_core",
    "OpenImageDenoise_device_cpu",
    "tbbbind",
    "tbbbind_2_0",
    "tbbbind_2_5",
)

# The facade library under .install/, per host — the name extern/oidn/CMakeLists.txt links by.
# The Unix archives ship it ONLY as a symlink onto a versioned real file.
# An install that drops links has no file at this path, and the build then fails at ninja rather than anywhere informative.
FACADE_LIBRARY = {
    "windows": "bin/OpenImageDenoise.dll",
    "linux": "bin/libOpenImageDenoise.so",
    "macos": "bin/libOpenImageDenoise.dylib",
}

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


@dataclass(frozen=True)
class Member:
    """One archive entry, already rerooted: a file's bytes, or the basename a symlink points at."""

    path: PurePosixPath
    data: bytes = b""
    link_to: str = ""

    @property
    def is_link(self) -> bool:
        return bool(self.link_to)


def install_members(members: list[Member], into: Path) -> int:
    """Writes every member the plan keeps, and reports how many that was.

    Symlinks are recreated as symlinks, and are written after every file so a link never precedes what it names.
    A host that refuses to create one is an error rather than a skip, since dropping the link is what leaves the facade library missing under the very name the build asks for.
    """
    written = 0
    links: list[tuple[Path, str]] = []
    for member in members:
        target = wanted(member.path)
        if target is None:
            continue
        out = into / Path(target)
        out.parent.mkdir(parents=True, exist_ok=True)
        if member.is_link:
            links.append((out, member.link_to))
        else:
            out.write_bytes(member.data)
        written += 1

    for out, link_to in links:
        try:
            out.symlink_to(link_to)
        except OSError as error:
            sys.exit(f"OIDN: cannot create the symlink {out} -> {link_to} ({error})")
    return written


def read_archive(asset: str, data: bytes) -> list[Member]:
    """Every file and symlink in the archive, with its single top-level directory stripped.

    Symlinks matter here rather than being an extraction detail.
    The Unix releases ship each library as one versioned real file plus the unversioned and soname links onto it, and those links are the names both CMake and the dynamic loader use.
    """
    out: list[Member] = []

    def add(name: str, payload: bytes = b"", link_to: str = "") -> None:
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts:
            sys.exit(f"refusing to extract {name!r} (path traversal)")
        if "/" in link_to or PurePosixPath(link_to).is_absolute():
            sys.exit(f"refusing to extract {name!r} (symlink escapes its directory: {link_to!r})")
        # Every release archive has one top-level directory named for the version.
        rerooted = PurePosixPath(*path.parts[1:]) if len(path.parts) > 1 else path
        out.append(Member(path=rerooted, data=payload, link_to=link_to))

    if asset.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for member in archive.infolist():
                if not member.is_dir():
                    add(member.filename, payload=archive.read(member))
    else:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as archive:
            for member in archive.getmembers():
                if member.issym():
                    add(member.name, link_to=member.linkname)
                elif member.isfile():
                    handle = archive.extractfile(member)
                    if handle is not None:
                        add(member.name, payload=handle.read())
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--force", action="store_true", help="re-download even when the pin already matches")
    args = parser.parse_args()

    upstream = deps_manifest.one(DEST)
    if not upstream.is_available:
        # Exit 0: a platform upstream does not build for is a fact about the release, not a failure of this run.
        # shaped-rendering sees no `oidn` target, compiles impl/oidn_null.cc and reports the member unsupported.
        print(f"OIDN publishes no release for {'/'.join(deps_manifest.host_keys())} — skipping")
        return

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
    # The names are checked rather than only the count, because a count cannot see a platform-specific miss.
    # An install can hold the expected number of members and still lack the facade library, or oneTBB entirely.
    missing = [name for name in LICENSE_MEMBERS.values() if not (staging / name).is_file()]
    facade = FACADE_LIBRARY[deps_manifest.host_os_key()]
    if not (staging / facade).exists():
        missing.append(facade)
    installed = sorted((staging / "bin").iterdir()) if (staging / "bin").is_dir() else []
    if not any(path.name.split(".")[0].removeprefix("lib") in TBB_STEMS for path in installed):
        missing.append("a oneTBB library")
    if written < 6 or missing:
        sys.exit(
            f"OIDN: the install plan kept {written} member(s) and is missing {missing or 'nothing'} — "
            f"the layout of {upstream.asset} is not what fetch-oidn.py expects."
        )

    (staging / "pin.txt").write_text(upstream.pin_hash + "\n", encoding="utf-8")

    if INSTALL.exists():
        shutil.rmtree(INSTALL)
    staging.rename(INSTALL)

    total = sum(f.stat().st_size for f in INSTALL.rglob("*") if f.is_file() and not f.is_symlink())
    print(f"OIDN installed into {INSTALL} ({written} files, {total / 1e6:.1f} MB)")
    print("Configure again to pick it up: uv run dev.py configure")


if __name__ == "__main__":
    main()
