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
import os
import shutil
import subprocess
from collections.abc import Sequence
from pathlib import Path

from .models import Preset


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


def compiler_defines(preset: Preset) -> dict[str, str]:
    """CMAKE_{C,CXX}_COMPILER overrides for a pinned clang/gcc toolset, or {} when none applies.

    MSVC pins via the vcvars environment, not cache variables, so it returns {} here.
    Raises ToolsetError if the requested compiler cannot be found.
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
    if not cxx or not cc:
        missing = cxx_name if not cxx else cc_name
        raise ToolsetError(
            f"--toolset {ts!r}: {missing!r} not found on PATH for the "
            f"{preset.family} preset {preset.name!r}"
        )
    return {"CMAKE_CXX_COMPILER": cxx, "CMAKE_C_COMPILER": cc}


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


def _validate(preset: Preset) -> None:
    """Eagerly fail (ToolsetError) if the preset's pinned toolset can't be resolved."""
    if preset.toolset is None:
        return
    if preset.family in ("clang", "gcc"):
        compiler_defines(preset)  # raises if not found
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
        _validate(new)
        out.append(new)
    return out
