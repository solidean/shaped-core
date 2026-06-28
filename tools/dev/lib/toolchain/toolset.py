"""Toolset pinning and build-directory overrides for --toolset / --build-suffix / --build-dir.

Presets only name a compiler *family* (clang / gcc / msvc); the concrete version is
otherwise whatever the environment defaults to. `--toolset` pins a specific one so the
same dev.py can drive, say, clang 19 / 20 / 21 — or two Visual Studio toolsets — on one
machine, each in its own build directory.

How a pinned toolset is applied depends on the family:
  - clang / gcc: override CMAKE_C/CXX_COMPILER with a versioned binary (see compiler_defines).
  - msvc: there is no compiler path (Ninja + cl from the injected env), so it is selected by
    Visual Studio instance + `-vcvars_ver` (see find_msvc_instance and process.msvc_env).

A requested toolset that cannot be found is a hard error (ToolsetError) raised eagerly when
overrides are applied, so the failure is a clean message rather than a confusing mid-build error.

Public API:
    apply_overrides(presets, ...) -> list[Preset]   # rewrite build_dir + attach toolset, validate
    compiler_defines(preset)      -> dict[str, str] # CMAKE_{C,CXX}_COMPILER for clang/gcc, else {}
    find_msvc_instance(toolset)   -> Path | None     # VS install whose VC/Tools/MSVC has the toolset
    ToolsetError
"""

from __future__ import annotations

import dataclasses
import json
import os
import platform
import re
import shutil
import subprocess
from collections.abc import Sequence
from pathlib import Path

from ..core.models import Preset
from ..project.presets import resolve_cache_variable


class ToolsetError(Exception):
    """Raised when a requested --toolset cannot be resolved, or overrides conflict."""


def _looks_like_path(value: str) -> bool:
    return "/" in value or "\\" in value


def _sanitize(value: str) -> str:
    """Fold a toolset value into a filesystem-safe tag for a build-dir suffix."""
    base = Path(value).name if _looks_like_path(value) else value
    return "".join(c if (c.isalnum() or c in "._-+") else "-" for c in base)


# ---------------------------------------------------------------------------
# clang / gcc: compiler-path overrides
# ---------------------------------------------------------------------------

def _versioned_names(family: str, version: str) -> tuple[str, str]:
    """(C++ driver, C driver) names for a bare version, e.g. ('clang++-21', 'clang-21')."""
    if family == "clang":
        return f"clang++-{version}", f"clang-{version}"
    return f"g++-{version}", f"gcc-{version}"  # gcc


def _sibling_c_compiler(cxx: Path) -> Path:
    """Derive the C driver next to an explicit C++ driver path (clang++ -> clang, g++ -> gcc)."""
    name = cxx.name
    for cxx_tok, c_tok in (("clang++", "clang"), ("g++", "gcc")):
        if cxx_tok in name:
            return cxx.with_name(name.replace(cxx_tok, c_tok))
    return cxx  # e.g. clang-cl drives both C and C++


def compiler_major(exe: str) -> int | None:
    """Major version of a compiler given by name or path (from its --version banner), or None."""
    if not (Path(exe).is_file() or shutil.which(exe)):
        return None
    try:
        out = subprocess.run([exe, "--version"], capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r"(\d+)\.\d+\.\d+", out.stdout)  # clang "... version 22.1.8"; gcc "... 14.3.0"
    return int(m.group(1)) if m else None


def compiler_defines(preset: Preset, root: Path | None = None) -> dict[str, str]:
    """CMAKE_{C,CXX}_COMPILER overrides for a pinned clang/gcc toolset, or {} when none applies.

    For a bare version N, prefer a versioned binary on PATH (`clang++-N` / `g++-N`) and override the
    compiler to it. When none exists — e.g. `clang-cl` on Windows or Homebrew's unversioned `clang++`
    on macOS — fall back to *asserting* that the preset's own compiler is major version N (no
    override): a hard error on mismatch, so a base-image compiler bump is loud. MSVC pins via the
    vcvars environment, not cache variables, so it returns {} here.
    """
    ts = preset.toolset
    if ts is None or preset.family not in ("clang", "gcc"):
        return {}

    if _looks_like_path(ts):
        cxx = Path(ts)
        if not cxx.is_file():
            raise ToolsetError(f"--toolset {ts!r}: no such compiler file")
        cc = _sibling_c_compiler(cxx)
        return {"CMAKE_CXX_COMPILER": str(cxx), "CMAKE_C_COMPILER": str(cc)}

    cxx_name, cc_name = _versioned_names(preset.family, ts)
    cxx, cc = shutil.which(cxx_name), shutil.which(cc_name)
    if cxx and cc:
        return {"CMAKE_CXX_COMPILER": cxx, "CMAKE_C_COMPILER": cc}

    # No versioned binary — assert the preset's configured compiler is itself major version `ts`.
    preset_cxx = resolve_cache_variable(root, preset.configure_preset, "CMAKE_CXX_COMPILER") if root else None
    if preset_cxx is not None and ts.isdigit():
        major = compiler_major(preset_cxx)
        if major is not None and major == int(ts):
            return {}  # the preset compiler already is this version; nothing to override
        if major is not None:
            raise ToolsetError(
                f"--toolset {ts}: the {preset.name!r} compiler {preset_cxx!r} is version {major}, not {ts}"
            )
    raise ToolsetError(
        f"--toolset {ts!r}: neither {cxx_name!r} on PATH nor a version-{ts} compiler for the "
        f"{preset.family} preset {preset.name!r}"
    )


# ---------------------------------------------------------------------------
# msvc: Visual Studio instance selection
# ---------------------------------------------------------------------------

def _vswhere() -> Path | None:
    vswhere = (
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    )
    return vswhere if vswhere.is_file() else None


def find_msvc_instance(toolset: str) -> Path | None:
    """Return the install path of a VS instance whose VC\\Tools\\MSVC has `toolset`, or None.

    `toolset` matches a toolset folder by prefix, so "14.51" selects "14.51.36231".
    Searches all instances including prerelease, so VS preview/insiders builds are reachable.
    """
    vswhere = _vswhere()
    if vswhere is None:
        return None
    result = subprocess.run(
        [str(vswhere), "-prerelease", "-all", "-property", "installationPath"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        return None
    for line in result.stdout.splitlines():
        inst = Path(line.strip())
        msvc_dir = inst / "VC" / "Tools" / "MSVC"
        if msvc_dir.is_dir() and any(d.name.startswith(toolset) for d in msvc_dir.iterdir()):
            return inst
    return None


def toolset_hint(folder: str) -> str:
    """The `--toolset` value that selects an MSVC toolset folder: its major.minor prefix.

    e.g. "14.51.36231" -> "14.51" (find_msvc_instance matches by prefix, and vcvars accepts it).
    """
    parts = folder.split(".")
    return ".".join(parts[:2]) if len(parts) >= 2 else folder


# ---------------------------------------------------------------------------
# Discovery (for `dev.py list-toolsets`)
# ---------------------------------------------------------------------------

def list_msvc_toolsets() -> list[dict]:
    """Every installed Visual Studio instance and the MSVC toolsets under each.

    One dict per instance: name, path, prerelease, and `toolsets` (full version folders,
    newest first). Empty off Windows or when vswhere is unavailable. Includes prerelease
    instances, so VS preview/insiders show up.
    """
    vswhere = _vswhere()
    if vswhere is None:
        return []
    result = subprocess.run(
        [str(vswhere), "-prerelease", "-all", "-format", "json", "-utf8"],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        return []
    try:
        instances = json.loads(result.stdout)
    except json.JSONDecodeError:
        return []

    out: list[dict] = []
    for inst in instances:
        path = Path(inst.get("installationPath", ""))
        msvc_dir = path / "VC" / "Tools" / "MSVC"
        toolsets = sorted((d.name for d in msvc_dir.iterdir() if d.is_dir()), reverse=True) \
            if msvc_dir.is_dir() else []
        out.append({
            "name": inst.get("displayName") or path.name,
            "path": str(path),
            "prerelease": bool(inst.get("isPrerelease", False)),
            "toolsets": toolsets,
        })
    out.sort(key=lambda i: i["name"])
    return out


def _compiler_version(exe: Path) -> str:
    """First line of `<exe> --version`, or '' if it can't be run."""
    try:
        result = subprocess.run([str(exe), "--version"], capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return ""
    line = result.stdout.splitlines()
    return line[0].strip() if line else ""


def _discover_drivers(driver: str) -> list[Path]:
    """Find `driver` and `driver-<version>` across PATH (e.g. clang++, clang++-21), PATH order.

    Deduplicated by file name (not target), so a bare driver and its versioned siblings are all
    listed even when they symlink to the same binary.
    """
    win = platform.system() == "Windows"
    exe_suffix = ".exe" if win else ""
    seen: dict[str, Path] = {}
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        d = Path(entry)
        if not d.is_dir():
            continue
        candidates = [d / f"{driver}{exe_suffix}", *d.glob(f"{driver}-*")]
        for c in candidates:
            if not c.is_file() or (win and c.suffix.lower() != ".exe"):
                continue
            seen.setdefault(c.name.lower(), c)
    return list(seen.values())


def _version_tag(name: str, driver: str) -> str | None:
    """The numeric --toolset value in a driver name, e.g. ('clang++-21', 'clang++') -> '21'.

    None for a bare driver ('clang++') or a non-numeric suffix ('clang-cl'), where there is no
    version to pass — the caller uses an explicit path instead.
    """
    stem = name[:-4] if name.lower().endswith(".exe") else name
    if not stem.lower().startswith(driver.lower()):
        return None
    rest = stem[len(driver):].lstrip("-")
    return rest if rest and rest[0].isdigit() else None


def list_compiler_toolsets(family: str) -> list[dict]:
    """Discover clang/gcc drivers on PATH for `family`.

    One dict per driver found: name, path, version (the --version banner), and `toolset` — the
    value to pass to --toolset (the trailing version in the name, e.g. clang++-21 -> "21"), or
    None for an unversioned driver (use an explicit path then).
    """
    drivers = {"clang": ("clang++", "clang-cl"), "gcc": ("g++",)}.get(family, ())
    out: list[dict] = []
    for driver in drivers:
        for exe in _discover_drivers(driver):
            out.append({
                "name": exe.name,
                "path": str(exe),
                "version": _compiler_version(exe),
                "toolset": _version_tag(exe.name, driver),
            })
    return out


def list_toolsets() -> dict[str, list[dict]]:
    """All discoverable toolsets grouped by compiler family (for `dev.py list-toolsets`)."""
    return {
        "msvc": list_msvc_toolsets(),
        "clang": list_compiler_toolsets("clang"),
        "gcc": list_compiler_toolsets("gcc"),
    }


def _validate(preset: Preset, root: Path) -> None:
    """Eagerly fail (ToolsetError) if the preset's pinned toolset can't be resolved."""
    if preset.toolset is None:
        return
    if preset.family in ("clang", "gcc"):
        compiler_defines(preset, root)  # raises if not found / version mismatch
    elif preset.family == "msvc":
        if not _looks_like_path(preset.toolset) and find_msvc_instance(preset.toolset) is None:
            raise ToolsetError(
                f"--toolset {preset.toolset!r}: no Visual Studio instance has MSVC toolset "
                f"{preset.toolset} (looked across released and prerelease installs)"
            )
    else:
        raise ToolsetError(
            f"--toolset is not supported for the {preset.family!r} preset {preset.name!r}"
        )


# ---------------------------------------------------------------------------
# Override application
# ---------------------------------------------------------------------------

def apply_overrides(
    presets: Sequence[Preset],
    *,
    root: Path,
    toolset: str | None = None,
    build_suffix: str | None = None,
    build_dir: str | None = None,
) -> list[Preset]:
    """Rewrite each preset's build_dir and attach the pinned toolset, validating eagerly.

    Build-dir precedence (highest first): --build-dir replaces the whole directory;
    --build-suffix appends `-<suffix>` to the preset folder; otherwise a pinned --toolset
    auto-derives `-<toolset>` so it never clobbers the default-toolset build's CMake cache.
    With no override the preset is returned unchanged.
    """
    if build_dir is not None and len(presets) > 1:
        raise ToolsetError("--build-dir names a single directory; it can't apply to multiple presets "
                           "(use --build-suffix for a toolset matrix)")

    out: list[Preset] = []
    for preset in presets:
        if build_dir is not None:
            bd = Path(build_dir)
            bd = bd if bd.is_absolute() else root / bd
        elif build_suffix is not None:
            bd = preset.build_dir.with_name(f"{preset.build_dir.name}-{build_suffix}")
        elif toolset is not None:
            bd = preset.build_dir.with_name(f"{preset.build_dir.name}-{_sanitize(toolset)}")
        else:
            bd = preset.build_dir

        new = dataclasses.replace(preset, build_dir=bd, toolset=toolset)
        _validate(new, root)
        out.append(new)
    return out
