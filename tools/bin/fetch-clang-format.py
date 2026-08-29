#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Download the pinned clang-format into tools/bin, so every dev formats with the one version the style file declares.

clang-format's output is not stable across major versions.
That is why `dev.py format` enforces the major from `.clang-format`'s `Requires: clang-format >= N` header, and refuses to run under any other.
That check is correct, and it used to be a dead end: a machine whose LLVM install is a different major had no path forward but a manual side-install.
This script is that path, and `dev.py format` runs it for you when the version is wrong or missing.

We follow the extern/ fetch model — a pinned artifact, a hash that is the authority, an idempotent pin file — with two deliberate differences.
The install lands in tools/bin rather than a .install/ directory, because it is one executable and tools/bin is already where our tool binaries live.
A .gitignore in that folder is what keeps the fetched one out of history.
And it lives here rather than under extern/, because nothing links it: it is a developer tool and not a dependency of the build, so it has no business in `deps list` or in the license manifest.

The binaries come from the `clang-format` PyPI wheels (the ssciwr/clang-format-wheel project), which repackage the official LLVM release binary per platform at ~1.5 MB.
That size is the whole reason not to use the LLVM release tarballs, which are hundreds of megabytes to extract one executable from.

Pinning is `_VERSION` plus the per-wheel SHA-256 table below, and a download is rejected unless its digest matches.
`_VERSION`'s major must equal what `.clang-format` requires, which the script checks rather than trusts.
So bumping the style file's `Requires:` without bumping here fails loudly instead of installing the wrong formatter.

Re-running is a no-op once tools/bin/.clang-format-pin.txt matches; pass --force to reinstall anyway.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import platform
import re
import stat
import sys
import urllib.request
import zipfile
from pathlib import Path

# This script lives in tools/bin/ and installs alongside itself.
BIN = Path(__file__).resolve().parent
ROOT = BIN.parent.parent
PIN_FILE = BIN / ".clang-format-pin.txt"

# The pinned release.
# Its major must match `.clang-format`'s `Requires: clang-format >= N`.
_VERSION = "22.1.8"

# SHA-256 per wheel, keyed by the platform tag in the file name.
# Only the platforms we support are listed: an unlisted host is a clear error rather than an unverified download.
# Refresh with: https://pypi.org/pypi/clang-format/<version>/json
_WHEELS = {
    "manylinux_2_27_x86_64.manylinux_2_28_x86_64": "b00cff6bfd1f1686f073a4fdf1cb937dbd58bf7510c659477805c03afdea0816",
    "manylinux_2_26_aarch64.manylinux_2_28_aarch64": "48c3b8dcfe9d4e964ced0e744e0f1f8ddc711bce92e50f6cab21e10f54857d08",
    "musllinux_1_2_x86_64": "734d22be5c9d3a72a841444817aae8168c8f4bdccb08de491f05673e42ec7304",
    "musllinux_1_2_aarch64": "02ff8ad2e6a60554cc6b9b34310f71adb04da39e94302e08e3a655b77e4dd31c",
    "macosx_11_0_arm64": "d1147107222c0dda3e4869e9e8c4a79f9ed1de83819e5274de42b82adf3d2129",
    "macosx_10_9_x86_64": "fc2ac5bd0ea41af49968fb69426207806d5f7016cb8f4bfbd44f4f1ffe8d53f2",
    "win_amd64": "5fe6ad3e9399d589aff5ead432568a84cdcbbd621f1708340819efd74cbf8176",
    "win_arm64": "1fac18f32426c6fd7acde7087511bd80e2c549b2cd7477099582c216ae82fa63",
}

_BASE_URL = "https://files.pythonhosted.org/packages/py2.py3/c/clang-format"


def installed_name() -> str:
    """The executable's name once installed, which is the plain tool name so a PATH-style lookup finds it."""
    return "clang-format.exe" if platform.system() == "Windows" else "clang-format"


def installed_path() -> Path:
    return BIN / installed_name()


def _is_musl() -> bool:
    """True on a musl libc (Alpine), whose wheels are a separate tag.

    Read from the interpreter's own shared-library list rather than platform.libc_ver(), which reports glibc for musl.
    """
    try:
        return "musl" in (Path("/proc/self/maps").read_text(encoding="utf-8", errors="replace"))
    except OSError:
        return False


def wheel_tag() -> str | None:
    """The `_WHEELS` key for this host, or None when we publish no pin for it."""
    system = platform.system()
    machine = platform.machine().lower()
    arm = machine in ("arm64", "aarch64")

    if system == "Windows":
        return "win_arm64" if arm else "win_amd64"
    if system == "Darwin":
        return "macosx_11_0_arm64" if arm else "macosx_10_9_x86_64"
    if system == "Linux":
        if _is_musl():
            return "musllinux_1_2_aarch64" if arm else "musllinux_1_2_x86_64"
        return "manylinux_2_26_aarch64.manylinux_2_28_aarch64" if arm else "manylinux_2_27_x86_64.manylinux_2_28_x86_64"
    return None


def required_major() -> int | None:
    """The major `.clang-format` declares, so the pin is checked against the style file rather than assumed to match it."""
    try:
        text = (ROOT / ".clang-format").read_text(encoding="utf-8")
    except OSError:
        return None
    m = re.search(r"Requires:\s*clang-format\s*>=\s*(\d+)", text)
    return int(m.group(1)) if m else None


def already_installed() -> bool:
    return PIN_FILE.is_file() and PIN_FILE.read_text(encoding="utf-8").strip() == _VERSION and installed_path().is_file()


def main() -> int:
    parser = argparse.ArgumentParser(description=f"install clang-format {_VERSION} into tools/bin")
    parser.add_argument("--force", action="store_true", help="reinstall even when the pin already matches")
    args = parser.parse_args()

    need = required_major()
    have_major = int(_VERSION.split(".")[0])
    if need is not None and need != have_major:
        print(
            f"clang-format: .clang-format requires major {need} but this script pins {_VERSION}. "
            "Bump _VERSION and the _WHEELS hashes together.",
            file=sys.stderr,
        )
        return 1

    if already_installed() and not args.force:
        return 0

    tag = wheel_tag()
    if tag is None:
        print(
            f"clang-format: no pinned wheel for {platform.system()}/{platform.machine()} — "
            f"install clang-format {have_major}.x yourself and put it on PATH.",
            file=sys.stderr,
        )
        return 1

    filename = f"clang_format-{_VERSION}-py2.py3-none-{tag}.whl"
    url = f"{_BASE_URL}/{filename}"
    expected = _WHEELS[tag]

    print(f"downloading clang-format {_VERSION} ({tag}) ...", file=sys.stderr)
    try:
        payload = urllib.request.urlopen(url, timeout=120).read()
    except Exception as e:  # noqa: BLE001 - any transport failure is the same story to the caller
        print(f"clang-format: download failed: {e}", file=sys.stderr)
        return 1

    actual = hashlib.sha256(payload).hexdigest()
    if actual != expected:
        print(f"clang-format: hash mismatch for {filename}\n  expected {expected}\n  actual   {actual}", file=sys.stderr)
        return 1

    # The wheel carries the executable at clang_format/data/bin/clang-format[.exe].
    member = "clang_format/data/bin/" + ("clang-format.exe" if tag.startswith("win") else "clang-format")
    try:
        with zipfile.ZipFile(io.BytesIO(payload)) as z:
            blob = z.read(member)
    except KeyError:
        print(f"clang-format: {filename} has no {member}", file=sys.stderr)
        return 1

    dest = installed_path()
    # Written via a temporary next to the target and then replaced, so a concurrent dev.py never sees a half-written binary.
    tmp = dest.with_suffix(dest.suffix + ".part")
    tmp.write_bytes(blob)
    tmp.chmod(tmp.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    os.replace(tmp, dest)
    PIN_FILE.write_text(_VERSION + "\n", encoding="utf-8")

    print(f"installed clang-format {_VERSION} -> {dest}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
