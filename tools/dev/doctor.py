"""Doctor: lightweight, read-only environment sanity checks.

Returns a list of (label, ok, detail) tuples so the caller decides how to print
and what exit code to use. No side effects.
"""

from __future__ import annotations

import json
import platform
import re
import shutil
import subprocess
from pathlib import Path

from . import clangd
from .presets import PresetError, load_presets
from .process import msvc_env


def _parse_version(text: str) -> tuple[int, ...] | None:
    """Extract the first dotted version number (e.g. '3.27.7') from text."""
    m = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
    if m is None:
        return None
    return tuple(int(g) for g in m.groups() if g is not None)


def required_cmake_version(root: Path) -> tuple[int, ...] | None:
    """The minimum CMake declared by the top-level CMakeLists.txt.

    Single source of truth: parses `cmake_minimum_required(VERSION X.Y[...])`
    so doctor enforces exactly what configure will require, with no second
    constant to keep in sync.
    """
    cml = root / "CMakeLists.txt"
    try:
        text = cml.read_text(encoding="utf-8")
    except OSError:
        return None
    m = re.search(r"cmake_minimum_required\s*\(\s*VERSION\s+([0-9.]+)", text, re.IGNORECASE)
    return _parse_version(m.group(1)) if m else None


def _cmake_check(root: Path) -> tuple[str, bool, str]:
    """Verify cmake is present and new enough to configure this project.

    Reporting cmake as merely 'found' is not enough: a too-old cmake fails
    configure with a cryptic 'CMake X required' error that the bare version
    check never surfaced. Compare against the declared minimum here instead.
    """
    exe = shutil.which("cmake")
    if exe is None:
        return ("cmake", False, "not found on PATH")
    try:
        out = subprocess.run(["cmake", "--version"], capture_output=True, text=True, timeout=15)
        first = out.stdout.splitlines()[0].strip() if out.stdout else exe
    except (OSError, subprocess.TimeoutExpired) as e:
        return ("cmake", False, f"failed to run ({e})")

    have = _parse_version(first)
    need = required_cmake_version(root)
    if have is not None and need is not None and have < need:
        need_str = ".".join(str(p) for p in need)
        return (
            "cmake",
            False,
            f"{first} too old - this project needs >= {need_str}; upgrade CMake (docs/requirements.md)",
        )
    return ("cmake", True, first)


def _clangd_checks(root: Path) -> list[tuple[str, bool, str]]:
    """Check that clangd is installed and can parse the project.

    Verifies, in order: clangd is found, the published compilation database
    (build/compile_commands.json — what the editor's clangd reads) exists, and
    clangd can `--check` a real file from it with no error diagnostics. The last
    step is what catches a missing/stale database, which silently breaks IDE
    code intelligence.
    """
    clangd_bin = clangd.find_clangd()
    if clangd_bin is None:
        return [("clangd", False, "not found on PATH (needed for IDE code intelligence)")]

    ver = clangd.version(clangd_bin) or clangd_bin
    checks: list[tuple[str, bool, str]] = [("clangd", True, ver)]

    cc_file = root / "build" / "compile_commands.json"
    if not cc_file.is_file():
        checks.append(
            ("clangd database", False, "build/compile_commands.json missing - run: uv run dev.py configure")
        )
        return checks
    checks.append(("clangd database", True, "build/compile_commands.json"))

    # Pick a real file from the database and confirm clangd parses it cleanly.
    try:
        entries = json.loads(cc_file.read_text(encoding="utf-8"))
        sample = Path(entries[0]["file"]) if entries else None
    except (OSError, ValueError, KeyError, IndexError):
        sample = None
    if sample is None or not sample.is_file():
        checks.append(("clangd check", False, "could not pick a sample file from the database"))
        return checks

    # Let clangd discover the database exactly as the editor does (via .clangd
    # and its upward search) — don't force --compile-commands-dir, or a broken
    # .clangd would be masked.
    try:
        result = clangd.check_file(clangd_bin, sample, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        checks.append(("clangd check", False, f"failed to run clangd --check ({e})"))
        return checks

    rel = sample.name
    if not result.found_database:
        checks.append(
            ("clangd check", False, f"clangd did not discover the database for {rel} - check .clangd CompilationDatabase")
        )
    elif result.ok:
        checks.append(("clangd check", True, f"{rel} parses cleanly ({len(result.warnings)} warning(s))"))
    else:
        checks.append(
            ("clangd check", False, f"{len(result.errors)} error(s) in {rel} - debug: uv run dev.py diagnose clangd <file>")
        )
    return checks


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

    checks.append(_cmake_check(root))

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

    checks.extend(_clangd_checks(root))

    return checks
