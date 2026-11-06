"""`install monado` — Monado at the commit `tools/monado.version` pins, built in ~/tools/monado/<commit>.

It is the OpenXR runtime shaped-xr's session tests run on, a simulated headset with no window, so they need no hardware.
Every `dev.py test` then selects it by itself; lib/toolchain/monado.py is that half, and why it stops at tests.

Monado has no prebuilt Windows runtime, so this builds it from source, with its dependencies through vcpkg.
A failed build keeps what it made, so running this again resumes it.
Windows only so far.
"""

from __future__ import annotations

import argparse
import os
import platform
import subprocess
from pathlib import Path

from tools.dev import console
from tools.dev.lib.core.process import msvc_env
from tools.dev.lib.toolchain import monado as mo
from tools.dev.lib.toolchain import vulkan_sdk as vk

from .context import Context

NAME = "monado"
DESCRIPTION = "Monado, a simulated headset that shaped-xr's session tests run on"

_MONADO_REPO = "https://gitlab.freedesktop.org/monado/monado.git"
_VCPKG_REPO = "https://github.com/microsoft/vcpkg.git"

_CONFIGURE_FLAGS = [
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
    "-DCMAKE_C_COMPILER=cl",
    "-DCMAKE_CXX_COMPILER=cl",
    # u_file.cpp reads __cplusplus to pick <filesystem>, and without this MSVC reports C++98 and takes a removed header.
    "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc /Zc:__cplusplus",
    "-DVCPKG_TARGET_TRIPLET=x64-windows",
    "-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON",
    "-DXRT_FEATURE_SERVICE=OFF",
    # The null compositor is what runs, but targets/openxr links comp_main whether or not it is built.
    "-DXRT_MODULE_COMPOSITOR_MAIN=ON",
    "-DBUILD_TESTING=OFF",
    "-DBUILD_DOC=OFF",
]


def list_state() -> None:
    commit = mo.pin()["monado"][:12]
    print(f"{console.bold(NAME)}  {DESCRIPTION}, pinned to {commit} in tools/monado.version")
    install = mo.install_dir()
    if mo.is_complete(install):
        print(f"  {console.green(f'{commit} at {install}')}")
    else:
        print(f"  {console.dim('not installed')}")


def _run(command: list[str], ctx: Context, cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    print(console.dim(f"  {' '.join(command)}"), flush=True)
    code = subprocess.run(command, cwd=cwd, env=env).returncode
    if code != 0:
        ctx.die(f"{command[0]} exited with {code}; running `dev.py install {NAME}` again resumes where this stopped")


def _head_of(checkout: Path) -> str | None:
    if not (checkout / ".git").exists():
        return None
    result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=checkout, capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else None


def _fetch_monado(checkout: Path, commit: str, ctx: Context) -> None:
    if _head_of(checkout) == commit:
        return
    checkout.mkdir(parents=True, exist_ok=True)
    if not (checkout / ".git").exists():
        _run(["git", "init", "--quiet"], ctx, cwd=checkout)
    _run(["git", "fetch", "--quiet", "--depth", "1", _MONADO_REPO, commit], ctx, cwd=checkout)
    _run(["git", "checkout", "--quiet", "--detach", "FETCH_HEAD"], ctx, cwd=checkout)


def _fetch_vcpkg(checkout: Path, commit: str, ctx: Context) -> None:
    # vcpkg reads the baseline Monado's manifest names out of its own history, so this clone cannot be shallow.
    if _head_of(checkout) != commit:
        if not (checkout / ".git").exists():
            _run(["git", "clone", "--quiet", _VCPKG_REPO, str(checkout)], ctx)
        _run(["git", "checkout", "--quiet", "--detach", commit], ctx, cwd=checkout)
    if not (checkout / "vcpkg.exe").is_file():
        _run(["cmd", "/c", str(checkout / "bootstrap-vcpkg.bat"), "-disableMetrics"], ctx, cwd=checkout)


def run(args: argparse.Namespace, ctx: Context) -> None:
    install = mo.install_dir()
    if args.uninstall:
        print(f"  Monado is a folder of its own, so nothing here deletes it; remove {install} to uninstall it")
        return

    if platform.system() != "Windows":
        ctx.die(f"`dev.py install {NAME}` builds on Windows so far; see chore-xr-monado-ci")
    if mo.is_complete(install):
        print(f"  {console.green('already installed')} Monado at {install}")
        return
    if not os.environ.get("VULKAN_SDK"):
        version, _ = vk.pin()
        ctx.die(f"Monado builds against the Vulkan SDK: `uv run dev.py install vulkan-sdk` installs {version}")

    pinned = mo.pin()
    source = install / "monado"
    vcpkg = install / "vcpkg"
    build = install / "build"
    _fetch_monado(source, pinned["monado"], ctx)
    _fetch_vcpkg(vcpkg, pinned["vcpkg"], ctx)

    env = msvc_env() or dict(os.environ)
    toolchain = vcpkg / "scripts" / "buildsystems" / "vcpkg.cmake"
    _run(["cmake", "-S", str(source), "-B", str(build), f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", *_CONFIGURE_FLAGS], ctx, env=env)
    _run(["ninja", "-C", str(build)], ctx, env=env)

    if not mo.is_complete(install):
        ctx.die(f"{build} holds no runtime after building; nothing `dev.py test` can select")
    print(f"  {console.green('installed')} Monado at {install}")
    print(console.dim("  `dev.py test` selects it by itself; XR_RUNTIME_JSON set to anything else wins over it"))
