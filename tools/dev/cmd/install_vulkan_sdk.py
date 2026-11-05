"""`install vulkan-sdk` — LunarG's Vulkan SDK at the version `tools/vulkan-sdk.version` pins, in ~/tools/vulkan-sdk/<version>.

Every dev.py run then points VULKAN_SDK at it unless VULKAN_SDK is set already, so the vulkan backend configures with no setup of its own;
lib/toolchain/vulkan_sdk.py is that half.

The download is checked against the pinned SHA-256 before anything runs or unpacks.
On Windows the SDK is LunarG's installer, run unattended into the pinned folder.
Its core component needs admin rights even there, so it is started again elevated, and the user confirms one prompt.
On Linux it is LunarG's tarball, unpacked; nothing there needs admin rights.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

from tools.dev import console
from tools.dev.lib.toolchain import vulkan_sdk as vk

from .context import Context

NAME = "vulkan-sdk"
DESCRIPTION = "LunarG's Vulkan SDK, which the vulkan backend and its validation layer build and run against"

_DOWNLOADS = "https://sdk.lunarg.com/sdk/download"
# Windows reports ERROR_ELEVATION_REQUIRED when a program whose manifest asks for admin rights is started without them.
_ERROR_ELEVATION_REQUIRED = 740


def _kit_file(version: str, kit: str) -> str:
    return f"vulkansdk-windows-X64-{version}.exe" if kit == "windows" else f"vulkansdk-linux-x86_64-{version}.tar.xz"


def list_state() -> None:
    version, _ = vk.pin()
    print(f"{console.bold(NAME)}  {DESCRIPTION}, pinned to {version} in tools/vulkan-sdk.version")
    installed = vk.installed_sdk_dir()
    if installed is not None:
        print(f"  {console.green(f'{version} at {installed}')}")
        return
    others = sorted(p.name for p in vk.default_root().iterdir() if p.is_dir()) if vk.default_root().is_dir() else []
    others = [o for o in others if not o.startswith(".")]
    if others:
        print(f"  {console.yellow(f'{', '.join(others)} in {vk.default_root()}, not the pinned {version}')}")
    else:
        print(f"  {console.dim('not installed')}")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def _download(url: str, dest: Path, expected_sha256: str, ctx: Context) -> None:
    """Streams `url` into `dest`, which is removed again unless its SHA-256 is the pinned one."""
    print(console.dim(f"  download {url}"), flush=True)
    request = urllib.request.Request(url, headers={"User-Agent": "shaped-core-dev"})
    digest = hashlib.sha256()
    try:
        with urllib.request.urlopen(request, timeout=300) as response, dest.open("wb") as out:  # noqa: S310 (fixed LunarG host)
            while chunk := response.read(1 << 20):
                digest.update(chunk)
                out.write(chunk)
    except OSError as e:
        dest.unlink(missing_ok=True)
        ctx.die(f"downloading {url} failed: {e}")
    if digest.hexdigest() != expected_sha256:
        dest.unlink(missing_ok=True)
        ctx.die(f"{dest.name} does not match the SHA-256 tools/vulkan-sdk.version pins; nothing was installed")


def _run_elevated(command: list[str]) -> int:
    """Starts `command` with admin rights, which asks the user to confirm, and returns its exit code once it ends."""

    def quoted(arg: str) -> str:
        # Start-Process joins ArgumentList with spaces as it is, so an argument holding one carries its own double quotes.
        return "'" + f'"{arg}"'.replace("'", "''") + "'"

    script = (f"$p = Start-Process -FilePath {quoted(command[0])} -ArgumentList @({', '.join(quoted(c) for c in command[1:])})"
              " -Verb RunAs -Wait -PassThru; exit $p.ExitCode")
    print(console.dim("  the installer asks for admin rights: confirm the prompt Windows shows"), flush=True)
    return subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script]).returncode


def _run_streamed(command: list[str]) -> tuple[int, bool]:
    """Runs `command` with its output passed through, and says whether it stopped for want of admin rights."""
    needs_admin = False
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace") as p:
        assert p.stdout is not None
        for line in p.stdout:
            sys.stdout.write(line)
            needs_admin = needs_admin or _ELEVATION_MESSAGE in line
    sys.stdout.flush()
    return p.returncode, needs_admin


# What the installer prints when its core component needs admin rights a command-line run cannot ask for.
# It unpacks everything first, then rolls the whole install back and exits with 1, so this line is the only tell.
_ELEVATION_MESSAGE = "Cannot elevate access rights while running from command line"


def _install_windows(installer: Path, target: Path, ctx: Context) -> None:
    # LunarG's documented unattended install; the folder must not exist yet.
    command = [str(installer), "--root", str(target), "--accept-licenses", "--default-answer", "--confirm-command", "install"]
    print(console.dim(f"  {' '.join(command)}"), flush=True)
    try:
        code, needs_admin = _run_streamed(command)
    except OSError as e:
        if getattr(e, "winerror", None) != _ERROR_ELEVATION_REQUIRED:
            ctx.die(f"starting the installer failed: {e}")
        code, needs_admin = 1, True
    if code != 0 and needs_admin:
        # The rolled-back attempt leaves no folder behind, which the installer needs for its second run.
        if target.exists():
            shutil.rmtree(target)
        code = _run_elevated(command)
    if code != 0:
        ctx.die(f"the Vulkan SDK installer exited with {code}")


def _install_linux(archive: Path, version: str, ctx: Context) -> None:
    # The tarball holds one folder named after the version, which is the install.
    staged = vk.default_root() / f".{version}.new"
    if staged.exists():
        shutil.rmtree(staged)
    staged.mkdir(parents=True)
    with tarfile.open(archive, "r:xz") as tf:
        tf.extractall(staged, filter="data")
    top = staged / version
    if not top.is_dir():
        ctx.die(f"{archive.name} holds no {version} folder")
    os.replace(top, vk.default_root() / version)
    shutil.rmtree(staged)


def run(args: argparse.Namespace, ctx: Context) -> None:
    version, digests = vk.pin()
    target = vk.default_root() / version
    if args.uninstall:
        print(f"  the Vulkan SDK is a folder rather than a link, so nothing here deletes it; remove {target} to uninstall it")
        if platform.system() == "Windows":
            print(console.dim(f"  {target / 'maintenancetool.exe'} purge also undoes what the installer registered"))
        return

    kit = vk.KITS.get(platform.system())
    if kit is None or platform.machine() not in ("AMD64", "x86_64"):
        ctx.die(f"LunarG publishes no Vulkan SDK kit dev.py installs for {platform.system()} {platform.machine()}; "
                "on macOS the metal backend needs none")

    sdk = vk.sdk_dir(version)
    if vk.is_complete(sdk):
        print(f"  {console.green('already installed')} Vulkan SDK {version} at {sdk}")
        return
    if target.exists():
        # an install that stopped halfway, and the installer refuses a folder that is not empty
        print(console.dim(f"  removing the incomplete install at {target}"), flush=True)
        shutil.rmtree(target)

    vk.default_root().mkdir(parents=True, exist_ok=True)
    name = _kit_file(version, kit)
    # Kept until an install succeeds, so a failed one is retried without downloading it again.
    download = vk.default_root() / f".{name}"
    if download.is_file() and _sha256(download) == digests[kit]:
        print(console.dim(f"  reusing the verified {download}"), flush=True)
    else:
        _download(f"{_DOWNLOADS}/{version}/{kit}/{name}", download, digests[kit], ctx)
    if kit == "windows":
        _install_windows(download, target, ctx)
    else:
        _install_linux(download, version, ctx)
    download.unlink(missing_ok=True)

    if not vk.is_complete(sdk):
        ctx.die(f"{sdk} holds no vulkan/vulkan.h and loader library after installing; nothing dev.py can point VULKAN_SDK at")
    print(f"  {console.green('installed')} Vulkan SDK {version} at {sdk}")
    if os.environ.get("VULKAN_SDK") and Path(os.environ["VULKAN_SDK"]) != sdk:
        print(console.yellow(f"  VULKAN_SDK={os.environ['VULKAN_SDK']} is set, and dev.py honours it over this install; "
                             "unset it to build against the pinned one"), file=sys.stderr)
    else:
        print(console.dim("  dev.py points VULKAN_SDK at it by itself; the next configure picks up the vulkan backend"))
