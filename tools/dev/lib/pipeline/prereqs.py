"""External prerequisites that must exist before CMake configures.

Currently just DXC: the (Windows-only) shaped-shader-compiler-dxc library links DXC, whose
prebuilt release binaries live under extern/dxc/.install/. They are not committed, so we fetch
them on demand — every Windows configure runs extern/dxc/download-dxc.py, which is a small,
fast download (a few seconds), then a cheap pin-file check on subsequent runs. Set SC_SKIP_DXC=1
to skip it (the library is then simply left unbuilt).
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
    """Read PIN_HASH from download-dxc.py (the authority the install is matched against)."""
    if not script.is_file():
        return None
    for line in script.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if s.startswith("PIN_HASH"):
            return s.split("=", 1)[1].strip().strip('"').strip("'")
    return None


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

    expected = _pinned_hash(script)
    pin = root / "extern" / "dxc" / ".install" / "pin.txt"
    if expected and pin.is_file() and pin.read_text(encoding="utf-8").strip() == expected:
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
