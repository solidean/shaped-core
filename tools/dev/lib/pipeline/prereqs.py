"""External prerequisites that must exist before CMake configures.

Four deps are fetched rather than committed — DXC, Zydis, SDL3 and SQLite — so every configure runs their fetch script.
That is seconds on a cold install and a cheap pin-file check after, and `SC_SKIP_<NAME>=1` opts out of one.
A cross-target preset skips all four, since these are host-side dependencies.
None of them is fatal: a failure leaves the dependent target unbuilt and configure proceeds.

DXC and Zydis are Windows-only, while SDL3 and SQLite run everywhere — which is what makes a cold Linux or macOS configure do real work.
Each dep's own docs own its pin, its size and what is missing without it — for Zydis that is tools/instruction-tracer/readme.md, not a libs/ doc.
"""

from __future__ import annotations

import os
import platform
import subprocess
import sys
from pathlib import Path

from ..core import profile

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
    with profile.span(name, type="prereq", extra={"script": script_name}):
        result = subprocess.run([sys.executable, str(script)], cwd=root)
    if result.returncode != 0:
        print(
            f"{name}: {script_name} failed — {dependent} will be skipped. "
            f"Run `uv run extern/{directory}/{script_name}` manually to see the error.",
            file=sys.stderr,
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
        windows_only=True,
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
