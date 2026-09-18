"""External prerequisites that must exist before CMake configures.

Four deps are fetched rather than committed — DXC, Zydis, SDL3 and SQLite — so every configure runs their fetch script.
That is seconds on a cold install and a cheap pin-file check after, and `SC_SKIP_<NAME>=1` opts out of one.
A cross-target preset skips all four, since these are host-side dependencies.
None of them is fatal: a failure leaves the dependent target unbuilt and configure proceeds.

Zydis is Windows-only; DXC is fetched on Windows and Linux, while SDL3 and SQLite run everywhere — which is what makes a cold Linux or macOS configure do real work.
Each dep's own docs own its pin, its size and what is missing without it — for Zydis that is tools/instruction-tracer/readme.md, not a libs/ doc.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
from pathlib import Path

from ..core import profile, ui
from ..core.process import emsdk_env
from ..project.pins import is_current

# Preset name fragments for cross-targets that never use these (host-side) dependencies.
_NON_NATIVE = ("wasm", "emscripten", "web", "android", "ios")


def _ensure(
    root: Path,
    preset_name: str,
    *,
    name: str,
    directory: str,
    script_name: str,
    skip_env: str,
    windows_only: bool,
    doing: str,
    dependent: str,
) -> None:
    """Run extern/<directory>/<script_name> when its install is missing or at the wrong pin.

    `doing` completes the "<name>: ..." progress line; `dependent` names what goes unbuilt on failure.
    The per-dep policy lives in the public wrappers below — this only carries it out.
    """
    if windows_only and platform.system() != "Windows":
        return
    if os.environ.get(skip_env):
        return
    if any(tag in preset_name.lower() for tag in _NON_NATIVE):
        return

    script = root / "extern" / directory / script_name
    if not script.is_file():
        return

    manifest = root / "extern" / directory / "dependency.yml"
    pin = root / "extern" / directory / ".install" / "pin.txt"
    if is_current(manifest, pin):
        return  # already installed at the pinned release — fast path

    ui.write_line(f"{name}: {doing} (set {skip_env}=1 to skip) ...")
    # Through `uv run`, not sys.executable: the script reads its pin from dependency.yml, so it needs the pyyaml its PEP 723 block declares.
    # Only a real fetch pays that resolution — the fast path above never gets here.
    # The fetch script inherits this terminal and narrates itself, so the region parks rather than counting lines it cannot see.
    # Without that, the next erase deletes as much of the script's own output as the frame was tall.
    with profile.span(name, type="prereq", extra={"script": script_name}), ui.suspend():
        result = subprocess.run(["uv", "run", str(script)], cwd=root)
    if result.returncode != 0:
        ui.write_line(
            f"{name}: {script_name} failed — {dependent} will be skipped. "
            f"Run `uv run extern/{directory}/{script_name}` manually to see the error."
        )


def ensure_dxc(root: Path, preset_name: str = "") -> None:
    """Download DXC into extern/dxc/.install when it is missing or at the wrong pin.

    A failure leaves shaped-shader-compiler-dxc unbuilt.
    """
    _ensure(
        root,
        preset_name,
        name="dxc",
        directory="dxc",
        script_name="download-dxc.py",
        skip_env="SC_SKIP_DXC",
        # Fetched on Windows and Linux both: the same release ships an asset for each, and vulkan needs the SPIR-V
        # half of the compiler.
        # macOS has no release asset upstream, which _ensure's own script and manifest checks already cover.
        windows_only=False,
        doing="downloading the pinned DirectX Shader Compiler release",
        dependent="shaped-shader-compiler-dxc",
    )


def ensure_zydis(root: Path, preset_name: str = "") -> None:
    """Generate the amalgamated Zydis into extern/zydis/.install when it is missing or at the wrong pin.

    A failure leaves the instruction-tracer tool unbuilt.
    """
    _ensure(
        root,
        preset_name,
        name="zydis",
        directory="zydis",
        script_name="fetch-zydis.py",
        skip_env="SC_SKIP_ZYDIS",
        windows_only=True,
        doing="fetching the pinned Zydis decoder for instruction-tracer",
        dependent="instruction-tracer",
    )


def ensure_sdl3(root: Path, preset_name: str = "") -> None:
    """Download the SDL3 source into extern/sdl3/.install when it is missing or at the wrong pin.

    A failure leaves shaped-rendering building without its window API.
    """
    _ensure(
        root,
        preset_name,
        name="sdl3",
        directory="sdl3",
        script_name="fetch-sdl3.py",
        skip_env="SC_SKIP_SDL3",
        windows_only=False,
        doing="downloading the pinned SDL3 source release for sr::window",
        dependent="shaped-rendering's window API",
    )


def ensure_sqlite(root: Path, preset_name: str = "") -> None:
    """Download the SQLite amalgamation into extern/sqlite/.install when it is missing or at the wrong pin.

    A failure leaves babel-serializer's SQLite format reporting the backend unavailable.
    """
    _ensure(
        root,
        preset_name,
        name="sqlite",
        directory="sqlite",
        script_name="fetch-sqlite.py",
        skip_env="SC_SKIP_SQLITE",
        windows_only=False,
        doing="downloading the pinned SQLite amalgamation for babel::sqlite",
        dependent="babel-serializer's SQLite format",
    )


def ensure_node_webgpu(root: Path, preset_name: str = "", emsdk_path: str | None = None) -> None:
    """Install the pinned `webgpu` npm package into tools/dev/js when a WebGPU wasm preset needs it and it is missing.

    Node has no WebGPU of its own; that package is Dawn's binding, which tools/dev/js/webgpu-preload.mjs installs as navigator.gpu.
    npm is searched on the emsdk overlay's PATH first, because emsdk bundles the node the tests run under and its npm, and stays off the user's PATH.
    A failure is not fatal: under node every GPU test then SKIPs for want of an adapter, and deno needs none of this.
    """
    if "webgpu" not in preset_name.lower() or os.environ.get("SC_SKIP_NODE_WEBGPU"):
        return
    js = root / "tools" / "dev" / "js"
    if (js / "node_modules" / "webgpu" / "package.json").is_file():
        return
    env = emsdk_env(emsdk_path) or dict(os.environ)
    npm = shutil.which("npm", path=env.get("PATH") or env.get("Path"))
    if npm is None:
        ui.write_line("node-webgpu: npm not found — node runs of WebGPU tests will SKIP (install emsdk or node, or use --runtime deno)")
        return

    ui.write_line("node-webgpu: installing the pinned webgpu npm package (set SC_SKIP_NODE_WEBGPU=1 to skip) ...")
    # npm starts through `#!/usr/bin/env node`, so it needs the overlay's PATH to find the node beside it.
    with profile.span("node-webgpu", type="prereq"), ui.suspend():
        result = subprocess.run([npm, "ci", "--no-audit", "--no-fund"], cwd=js, env=env)
    if result.returncode != 0:
        ui.write_line("node-webgpu: npm ci failed — node runs of WebGPU tests will SKIP. Run `npm ci` in tools/dev/js to see the error.")
