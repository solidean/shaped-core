"""Doctor: lightweight, read-only environment sanity checks.

Returns a list of (label, ok, detail) tuples so the caller decides how to print
and what exit code to use. No side effects.
"""

from __future__ import annotations

import platform
import shutil
import subprocess
from pathlib import Path

from .presets import PresetError, load_presets
from .process import msvc_env


def _tool_version(name: str) -> tuple[bool, str]:
    exe = shutil.which(name)
    if exe is None:
        return False, "not found on PATH"
    try:
        out = subprocess.run([name, "--version"], capture_output=True, text=True, timeout=15)
        first = out.stdout.splitlines()[0] if out.stdout else exe
        return True, first.strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, f"failed to run ({e})"


def doctor(root: Path, default_preset: str | None = None) -> list[tuple[str, bool, str]]:
    """Run sanity checks and return (label, ok, detail) for each."""
    checks: list[tuple[str, bool, str]] = []

    ok, detail = _tool_version("cmake")
    checks.append(("cmake", ok, detail))

    ok, detail = _tool_version("ninja")
    checks.append(("ninja", ok, detail))

    if platform.system() == "Windows":
        env = msvc_env()
        # None means either cl.exe already on PATH (fine) or not found.
        compiler_ok = env is not None or shutil.which("cl") is not None or shutil.which("clang-cl") is not None
        detail = "MSVC env reachable" if compiler_ok else "no MSVC/clang-cl compiler found"
        checks.append(("compiler", compiler_ok, detail))
    else:
        cxx = shutil.which("clang++") or shutil.which("g++")
        checks.append(("compiler", cxx is not None, cxx or "no clang++/g++ on PATH"))

    try:
        presets = load_presets(root)
        names = [p.name for p in presets]
        checks.append(("presets parse", True, f"{len(names)} build preset(s)"))
        if default_preset is not None:
            present = default_preset in names
            detail = default_preset if present else f"{default_preset!r} not among build presets"
            checks.append(("default preset", present, detail))
    except PresetError as e:
        checks.append(("presets parse", False, str(e)))

    return checks
