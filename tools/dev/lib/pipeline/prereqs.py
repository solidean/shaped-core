"""External prerequisites that must exist before CMake configures.

Three deps are fetched rather than committed, so we hydrate them on demand.
Each configure runs the fetch script: a few seconds on a cold install, a cheap pin-file check after.
None is fatal — a failure leaves the dependent target unbuilt and configure proceeds.

- DXC: the (Windows-only) shaped-shader-compiler-dxc library links its prebuilt release
  binaries from extern/dxc/.install/. Set SC_SKIP_DXC=1 to skip.
- Zydis: the (Windows-only) instruction-tracer tool links the amalgamated decoder generated
  into extern/zydis/.install/ — ~12 MB of generated tables we keep out of the history. Set
  SC_SKIP_ZYDIS=1 to skip.
- SDL3: shaped-rendering's sr::window is built on the source release fetched into
  extern/sdl3/.install/. Set SC_SKIP_SDL3=1 to skip.

SDL3 is the one that runs on every platform, so it is also the one that makes a cold Linux or macOS
configure do real work: ~15 MB downloaded, then SDL compiled once (~35 s per preset on Windows).
Both are cached afterwards, by pin.txt and by the build tree respectively.
"""

from __future__ import annotations

import os
import platform
import subprocess
import sys
from pathlib import Path

# Preset name fragments for cross-targets that never use these (host-side) dependencies.
_NON_NATIVE = ("wasm", "emscripten", "web", "android", "ios")


def _pinned_hash(script: Path) -> str | None:
    """Read PIN_HASH from a fetch script (the authority its install is matched against)."""
    if not script.is_file():
        return None
    for line in script.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if s.startswith("PIN_HASH"):
            return s.split("=", 1)[1].strip().strip('"').strip("'")
    return None


def _is_current(script: Path, pin: Path) -> bool:
    """True when the install's pin.txt already matches the script's PIN_HASH."""
    expected = _pinned_hash(script)
    return bool(expected) and pin.is_file() and pin.read_text(encoding="utf-8").strip() == expected


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

    pin = root / "extern" / directory / ".install" / "pin.txt"
    if _is_current(script, pin):
        return  # already installed at the pinned release — fast path

    print(f"{name}: {doing} (set {skip_env}=1 to skip) ...", file=sys.stderr)
    result = subprocess.run([sys.executable, str(script)], cwd=root)
    if result.returncode != 0:
        print(
            f"{name}: {script_name} failed — {dependent} will be skipped. "
            f"Run `uv run extern/{directory}/{script_name}` manually to see the error.",
            file=sys.stderr,
        )


def ensure_dxc(root: Path, preset_name: str = "") -> None:
    """Download DXC into extern/dxc/.install when it is missing or at the wrong pin. No-op off
    Windows, for cross-target presets, when SC_SKIP_DXC is set, or when the install is already
    current. A failure is reported but not fatal — configure proceeds and simply skips the library."""
    _ensure(
        root,
        preset_name,
        name="dxc",
        directory="dxc",
        script_name="download-dxc.py",
        skip_env="SC_SKIP_DXC",
        windows_only=True,
        doing="downloading the pinned DirectX Shader Compiler release",
        dependent="shaped-shader-compiler-dxc",
    )


def ensure_zydis(root: Path, preset_name: str = "") -> None:
    """Generate the amalgamated Zydis into extern/zydis/.install when it is missing or at the
    wrong pin. No-op off Windows, for cross-target presets, when SC_SKIP_ZYDIS is set, or when
    the install is already current. A failure is reported but not fatal — configure proceeds and
    simply skips the instruction-tracer tool."""
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
    Runs on every platform, unlike DXC and Zydis. No-op for cross-target presets, when SC_SKIP_SDL3
    is set, or when the install is already current. A failure is reported but not fatal — configure
    proceeds and shaped-rendering simply builds without its window API."""
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
