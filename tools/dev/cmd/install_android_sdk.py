"""`install android-sdk` — the NDK, platform-tools, build-tools and one platform, as `tools/android-sdk.version` pins them, in ~/tools/android-sdk.

Every dev.py run then points ANDROID_NDK_ROOT at it unless that is set already; lib/toolchain/android_sdk.py is that half.

**Nothing runs this for you, and it refuses to run without `--accept-license`.**
The archives are under Google's Android SDK license, and accepting it is a person's decision, never a build's.

This fetches the archives sdkmanager would, straight from Google's repository, because sdkmanager needs a JVM and installing the SDK should not.
Each is checked against the pinned SHA-1 before it is unpacked.
A finished archive is kept, so a failed install resumes at the next one.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import stat
import sys
import urllib.request
import zipfile
from pathlib import Path

from tools.dev import console
from tools.dev.lib.toolchain import android_sdk as sdk

from .context import Context

NAME = "android-sdk"
DESCRIPTION = "the Android NDK and SDK tools the Android presets and APKs build with"
LICENSE_URL = "https://developer.android.com/studio/terms"

_REPOSITORY = "https://dl.google.com/android/repository"


def list_state() -> None:
    print(f"{console.bold(NAME)}  {DESCRIPTION}, pinned in tools/android-sdk.version")
    root = sdk.installed_root()
    print(f"  {console.green(f'at {root}')}" if root is not None else f"  {console.dim('not installed')}")


def _sha1(path: Path) -> str:
    digest = hashlib.sha1()  # noqa: S324 (the only checksum Google's manifest publishes)
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def _download(url: str, dest: Path, ctx: Context) -> None:
    print(console.dim(f"  download {url}"), flush=True)
    request = urllib.request.Request(url, headers={"User-Agent": "shaped-core-dev"})
    try:
        with urllib.request.urlopen(request, timeout=300) as response, dest.open("wb") as out:  # noqa: S310 (fixed Google host)
            shutil.copyfileobj(response, out, 1 << 20)
    except OSError as e:
        dest.unlink(missing_ok=True)
        ctx.die(f"downloading {url} failed: {e}")


def extract(archive_path: Path, into: Path) -> Path:
    """Unpacks `archive_path` under `into` and returns the one folder it holds.

    zipfile.extractall drops what the NDK's Unix archives rely on: executable bits are stored but not applied, and a symlink comes out as a text file holding its target.
    """
    with zipfile.ZipFile(archive_path) as archive:
        tops = set()
        for info in archive.infolist():
            tops.add(info.filename.split("/")[0])
            mode = info.external_attr >> 16
            target = into / info.filename
            if stat.S_ISLNK(mode) and os.name != "nt":
                target.parent.mkdir(parents=True, exist_ok=True)
                os.symlink(archive.read(info).decode(), target)
                continue
            extracted = Path(archive.extract(info, into))
            if mode != 0 and os.name != "nt":
                extracted.chmod(stat.S_IMODE(mode))
    if len(tops) != 1:
        raise ValueError(f"{archive_path.name} holds {len(tops)} top-level entries where one folder was expected")
    return into / tops.pop()


def install_archive(a: sdk.archive, root: Path, ctx: Context) -> None:
    target = root / a.folder
    if target.is_dir():
        return
    download = root / f".{a.file}"
    if not (download.is_file() and _sha1(download) == a.sha1):
        _download(f"{_REPOSITORY}/{a.file}", download, ctx)
        if _sha1(download) != a.sha1:
            download.unlink(missing_ok=True)
            ctx.die(f"{a.file} does not match the SHA-1 tools/android-sdk.version pins; nothing of it was installed")

    staging = root / ".staging"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir()
    print(console.dim(f"  unpack {a.file}"), flush=True)
    top = extract(download, staging)
    target.parent.mkdir(parents=True, exist_ok=True)
    os.replace(top, target)
    shutil.rmtree(staging)
    download.unlink()


def run(args: argparse.Namespace, ctx: Context) -> None:
    root = sdk.default_root()
    if args.uninstall:
        print(f"  the Android SDK is a folder rather than a link, so nothing here deletes it; remove {root} to uninstall it")
        return

    host = sdk.host_name()
    if host is None:
        ctx.die("Google publishes no Android SDK for this system")
    archives = sdk.for_host(sdk.pin(), host)
    if sdk.is_installed(root, archives):
        print(f"  {console.green('already installed')} Android SDK at {root}")
        return
    if not args.accept_license:
        ctx.die(f"the Android SDK is under Google's license, {LICENSE_URL}; "
                f"read it, then run `uv run dev.py install {NAME} --accept-license`")

    root.mkdir(parents=True, exist_ok=True)
    for a in archives:
        install_archive(a, root, ctx)

    print(f"  {console.green('installed')} Android SDK at {root}")
    if os.environ.get("ANDROID_NDK_ROOT"):
        print(console.yellow(f"  ANDROID_NDK_ROOT={os.environ['ANDROID_NDK_ROOT']} is set, and dev.py honours it over this install"),
              file=sys.stderr)
    else:
        print(console.dim("  dev.py points ANDROID_NDK_ROOT at it by itself"))
