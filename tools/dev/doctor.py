"""Doctor: lightweight, read-only environment sanity checks.

Returns a list of (label, ok, detail) tuples so the caller decides how to print
and what exit code to use. No side effects.
"""

from __future__ import annotations

import json
import os
import platform
import re
import shutil
import subprocess
from pathlib import Path

from . import clangd
from .coverage import find_tool, resolve_tool
from .presets import PresetError, load_presets
from .process import emsdk_env, emsdk_toolchain_file, find_emsdk_root, msvc_env


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


def _pick_sample_source(entries: list[dict]) -> Path | None:
    """Pick a representative first-party TU for the clangd smoke test.

    Prefers a clean-core source over vendored/extern code (e.g. mimalloc's
    static.c): clangd diagnostics on third-party translation units aren't ours to
    fix, so they must not drive the toolchain verdict. Falls back to any libs/
    source, then anything in the database.
    """
    files = [Path(e["file"]) for e in entries if "file" in e]

    def score(p: Path) -> int:
        s = p.as_posix().lower()
        if "/extern/" in s:
            return 3  # vendored — avoid
        if "/libs/base/clean-core/" in s and p.suffix == ".cc":
            return 0
        if "/libs/" in s and p.suffix == ".cc":
            return 1
        return 2

    files.sort(key=score)
    return files[0] if files else None


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
        sample = _pick_sample_source(entries)
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


def _coverage_tool_check(
    label: str, name: str, env_var: str, build_dir: Path | None
) -> tuple[str, bool, str]:
    """Check an llvm-* coverage tool, resolved exactly as `dev.py coverage` does.

    Looks on PATH / via `env_var`, then beside the configured compiler (where
    clang-cl ships them on Windows). Reports the version line so a mismatch with
    the compiler — which silently corrupts `llvm-cov` mapping — is visible.
    """
    resolved = resolve_tool(name, env_var, build_dir) if build_dir else find_tool(name, env_var)
    if resolved is None:
        return (label, False,
                f"not found (needed for `dev.py coverage`; install LLVM or set {env_var})")
    try:
        out = subprocess.run([resolved, "--version"], capture_output=True, text=True, timeout=15)
        ver = next((ln.strip() for ln in out.stdout.splitlines() if "version" in ln.lower()), resolved)
        return (label, True, ver)
    except (OSError, subprocess.TimeoutExpired) as e:
        return (label, False, f"failed to run ({e})")


def _emscripten_checks(emsdk_path: str | None) -> list[tuple[str, bool | None, str]]:
    """Validate the Emscripten/emsdk toolchain used by the wasm-emscripten-* presets.

    Emscripten is an optional (Tier 2) target, so this stays advisory: when nothing
    signals intent to use it (no --emsdk-path, no SC_EMSDK_PATH/EMSDK, no emcc on
    PATH) it reports a single passing "not configured" line rather than failing a
    native-only developer's doctor run. Once any of those signals is present it
    validates strictly: emsdk located, emcc runnable, toolchain file present, and
    emsdk's node reachable — the things a WASM configure/build/test actually needs.
    """
    intent = bool(emsdk_path) or bool(os.environ.get("SC_EMSDK_PATH")) \
        or bool(os.environ.get("EMSDK")) or shutil.which("emcc") is not None
    if not intent:
        return [("emscripten", None,
                 "not configured (optional) - install emsdk and pass --emsdk-path for WASM presets")]

    root = find_emsdk_root(emsdk_path)
    if root is None:
        return [("emscripten", False,
                 "emsdk requested but not located - check --emsdk-path / SC_EMSDK_PATH / EMSDK "
                 "(expected an emsdk dir with emsdk_env)")]

    checks: list[tuple[str, bool | None, str]] = [("emsdk", True, str(root))]

    toolchain = emsdk_toolchain_file(root)
    checks.append(
        ("emsdk toolchain", toolchain.is_file(),
         str(toolchain) if toolchain.is_file() else f"missing {toolchain} - run: emsdk install latest")
    )

    # Resolve emcc/node through the emsdk environment (not just the ambient PATH),
    # so an un-activated but present emsdk still validates green.
    env = emsdk_env(emsdk_path)
    search_path = env.get("PATH") if env else None
    for tool, hint in (("emcc", "emsdk install/activate latest"), ("node", "bundled with emsdk")):
        exe = shutil.which(tool, path=search_path)
        if exe is None:
            checks.append((f"emscripten {tool}", False, f"not reachable via emsdk env ({hint})"))
            continue
        try:
            out = subprocess.run([exe, "--version"], capture_output=True, text=True, timeout=30)
            first = out.stdout.splitlines()[0].strip() if out.stdout else exe
            checks.append((f"emscripten {tool}", True, first))
        except (OSError, subprocess.TimeoutExpired) as e:
            checks.append((f"emscripten {tool}", False, f"failed to run ({e})"))

    return checks


def doctor(
    root: Path, default_preset: str | None = None, emsdk_path: str | None = None
) -> list[tuple[str, bool | None, str]]:
    """Run sanity checks and return (label, ok, detail) for each.

    `ok` is True (pass), False (fail), or None for an advisory check that neither
    passes nor fails — e.g. an optional toolchain that simply isn't configured.
    """
    checks: list[tuple[str, bool | None, str]] = []

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

    cov_build_dir: Path | None = None
    try:
        presets = load_presets(root)
        names = [p.name for p in presets]
        checks.append(("presets parse", True, f"{len(names)} build preset(s)"))
        if default_preset is not None:
            present = default_preset in names
            detail = default_preset if present else f"{default_preset!r} not among build presets"
            checks.append(("default preset", present, detail))
        # Any configured build dir lets us resolve the llvm tools beside the
        # compiler, matching how `dev.py coverage` finds them.
        cov_build_dir = next(
            (p.build_dir for p in presets if (p.build_dir / "CMakeCache.txt").is_file()), None
        )
    except PresetError as e:
        checks.append(("presets parse", False, str(e)))

    # Coverage toolchain (llvm-profdata / llvm-cov), needed for `dev.py coverage`.
    checks.append(_coverage_tool_check("llvm-profdata", "llvm-profdata", "LLVM_PROFDATA", cov_build_dir))
    checks.append(_coverage_tool_check("llvm-cov", "llvm-cov", "LLVM_COV", cov_build_dir))

    # Emscripten/WASM toolchain (optional; advisory unless emsdk is signalled).
    checks.extend(_emscripten_checks(emsdk_path))

    checks.extend(_clangd_checks(root))

    return checks
