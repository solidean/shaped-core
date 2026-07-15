"""External prerequisites that must exist before CMake configures.

Two deps are fetched rather than committed, so we hydrate them on demand — each configure runs
the fetch script, which is a few seconds on a cold install and a cheap pin-file check after
that. Neither is fatal: a failure leaves the dependent target unbuilt and configure proceeds.

- DXC: the (Windows-only) shaped-shader-compiler-dxc library links its prebuilt release
  binaries from extern/dxc/.install/. Set SC_SKIP_DXC=1 to skip.
- Zydis: the (Windows-only) instruction-tracer tool links the amalgamated decoder generated
  into extern/zydis/.install/ — ~12 MB of generated tables we keep out of the history. Set
  SC_SKIP_ZYDIS=1 to skip.
"""

from __future__ import annotations

import os
import platform
import subprocess
import sys
from pathlib import Path

# Preset name fragments for cross-targets that never use the (host, Windows-only) DXC.
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


def ensure_dxc(root: Path, preset_name: str = "") -> None:
    """Download DXC into extern/dxc/.install when it is missing or at the wrong pin. No-op off
    Windows, for cross-target presets, when SC_SKIP_DXC is set, or when the install is already
    current. A failure is reported but not fatal — configure proceeds and simply skips the library."""
    if platform.system() != "Windows":
        return
    if os.environ.get("SC_SKIP_DXC"):
        return
    if any(tag in preset_name.lower() for tag in _NON_NATIVE):
        return

    script = root / "extern" / "dxc" / "download-dxc.py"
    if not script.is_file():
        return

    pin = root / "extern" / "dxc" / ".install" / "pin.txt"
    if _is_current(script, pin):
        return  # already installed at the pinned release — fast path

    print("dxc: downloading the pinned DirectX Shader Compiler release (set SC_SKIP_DXC=1 to skip) ...",
          file=sys.stderr)
    result = subprocess.run([sys.executable, str(script)], cwd=root)
    if result.returncode != 0:
        print(
            "dxc: download-dxc.py failed — shaped-shader-compiler-dxc will be skipped. "
            "Run `uv run extern/dxc/download-dxc.py` manually to see the error.",
            file=sys.stderr,
        )


def ensure_zydis(root: Path, preset_name: str = "") -> None:
    """Generate the amalgamated Zydis into extern/zydis/.install when it is missing or at the
    wrong pin. No-op off Windows, for cross-target presets, when SC_SKIP_ZYDIS is set, or when
    the install is already current. A failure is reported but not fatal — configure proceeds and
    simply skips the instruction-tracer tool."""
    if platform.system() != "Windows":
        return
    if os.environ.get("SC_SKIP_ZYDIS"):
        return
    if any(tag in preset_name.lower() for tag in _NON_NATIVE):
        return

    script = root / "extern" / "zydis" / "fetch-zydis.py"
    if not script.is_file():
        return

    pin = root / "extern" / "zydis" / ".install" / "pin.txt"
    if _is_current(script, pin):
        return  # already installed at the pinned commit — fast path

    print("zydis: fetching the pinned Zydis decoder for instruction-tracer "
          "(set SC_SKIP_ZYDIS=1 to skip) ...", file=sys.stderr)
    result = subprocess.run([sys.executable, str(script)], cwd=root)
    if result.returncode != 0:
        print(
            "zydis: fetch-zydis.py failed — instruction-tracer will be skipped. "
            "Run `uv run extern/zydis/fetch-zydis.py` manually to see the error.",
            file=sys.stderr,
        )
