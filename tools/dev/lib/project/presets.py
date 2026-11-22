"""Load and resolve presets from CMakePresets.json.

Public API:
    load_presets(root)            -> list[Preset]   (all non-hidden build presets)
    resolve_presets(root, specs)  -> list[Preset]   (filtered by comma/glob/repeat)

Hidden configure presets are inheritance scaffolding and are never returned; a
build preset's CMAKE_BUILD_TYPE is resolved by walking its configure preset's
`inherits` chain.
A presets file's `include`s are followed, so a preset may come from an included file.
"""

from __future__ import annotations

import fnmatch
import json
import os
from collections.abc import Sequence
from pathlib import Path

from ..core.models import Preset


class PresetError(Exception):
    """Raised when a preset spec cannot be resolved or the file is malformed."""


def _load_with_includes(path: Path, seen: list[Path]) -> list[tuple[Path, dict]]:
    """`path` and every file its `include` names, transitively, each relative to the file naming it; included files first."""
    # Absolute but not resolved: a submodule reached through a link stays under the project's root.
    path = Path(os.path.abspath(path))
    if path in seen:
        return []
    seen.append(path)
    if not path.is_file():
        raise PresetError(f"presets file not found at {path}")
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except json.JSONDecodeError as e:
        raise PresetError(f"{path}: {e}") from e
    files: list[tuple[Path, dict]] = []
    for inc in data.get("include", []):
        files += _load_with_includes(path.parent / inc, seen)
    files.append((path, data))
    return files


def presets_files(root: Path) -> list[Path]:
    """The project's CMakePresets.json and every presets file it includes; empty when the project has none."""
    if not (root / "CMakePresets.json").is_file():
        return []
    try:
        return [p for p, _ in _load_with_includes(root / "CMakePresets.json", [])]
    except PresetError:
        return [root / "CMakePresets.json"]


def _read_presets_file(root: Path) -> dict:
    """The project's presets, with what its `include`s bring merged in, as CMake reads them.

    A project consuming shaped-core includes `extern/shaped-core/CMakePresets.json` rather than copying it, so a preset may live in either file.
    """
    path = root / "CMakePresets.json"
    if not path.is_file():
        raise PresetError(f"CMakePresets.json not found at {path}")
    merged: dict = {"configurePresets": [], "buildPresets": []}
    for _, data in _load_with_includes(path, []):
        for key in ("configurePresets", "buildPresets"):
            merged[key] += data.get(key, [])
    return merged


def _configure_presets_by_name(data: dict) -> dict[str, dict]:
    return {cp["name"]: cp for cp in data.get("configurePresets", [])}


def _resolve_cache_var(name: str, configs: dict[str, dict], var: str) -> str | None:
    """Resolve a cacheVariable by walking the inherits chain (depth-first, first wins)."""
    seen: set[str] = set()
    stack = [name]
    while stack:
        cur = stack.pop(0)
        if cur in seen or cur not in configs:
            continue
        seen.add(cur)
        cp = configs[cur]
        val = cp.get("cacheVariables", {}).get(var)
        if val is not None:
            # cacheVariables entries may be a bare string or {"value": ...}
            return val["value"] if isinstance(val, dict) else val
        inherits = cp.get("inherits", [])
        if isinstance(inherits, str):
            inherits = [inherits]
        stack = list(inherits) + stack  # parents take precedence in declared order
    return None


def resolve_cache_variable(root: Path, configure_preset: str, var: str) -> str | None:
    """Resolve a configure preset's cacheVariable, walking its `inherits` chain.

    Returns the first value found, parents in declared order, or None.
    doctor uses it to report the compiler a preset actually configures — the Homebrew-LLVM path on macOS, say — rather than whatever bare `clang++`/`g++` is on PATH.
    """
    data = _read_presets_file(root)
    configs = _configure_presets_by_name(data)
    return _resolve_cache_var(configure_preset, configs, var)


# Default build preset per platform: what dev.py builds when no --preset is given, and where host tools are built.
DEFAULT_BUILD_PRESETS: dict[str, str] = {
    "Windows": "relwithdebinfo-clang",
    "Linux": "relwithdebinfo-linux-clang",
    "Darwin": "macos-arm-llvm-relwithdebinfo",
}


def load_presets(root: Path) -> list[Preset]:
    """Return all non-hidden build presets, in declaration order."""
    data = _read_presets_file(root)
    configs = _configure_presets_by_name(data)

    presets: list[Preset] = []
    for bp in data.get("buildPresets", []):
        if bp.get("hidden"):
            continue
        name = bp["name"]
        cfg = bp.get("configurePreset")
        if cfg is None:
            raise PresetError(f"Build preset {name!r} has no configurePreset")
        build_type = _resolve_cache_var(cfg, configs, "CMAKE_BUILD_TYPE") or ""
        presets.append(
            Preset(
                name=name,
                configure_preset=cfg,
                build_dir=root / "build" / cfg,
                build_type=build_type,
            )
        )
    return presets


def resolve_presets(root: Path, specs: Sequence[str]) -> list[Preset]:
    """Resolve preset specs into a deduplicated list of Presets.

    Each spec may be a comma-separated list and/or a shell-style wildcard;
    `specs` itself may contain repeated entries (e.g. from `--preset a --preset b`).
    An empty `specs` returns [] so the caller can substitute a default.
    """
    available = load_presets(root)
    by_name = {p.name: p for p in available}

    # Flatten comma-lists across all repeated flags.
    patterns: list[str] = []
    for spec in specs:
        patterns.extend(s.strip() for s in spec.split(",") if s.strip())
    if not patterns:
        return []

    selected: list[Preset] = []
    seen: set[str] = set()
    for pat in patterns:
        if pat in by_name:
            matches = [by_name[pat]]
        else:
            matches = [p for p in available if fnmatch.fnmatch(p.name, pat)]
        if not matches:
            raise PresetError(
                f"No build preset matches {pat!r}. "
                f"Available: {', '.join(p.name for p in available)}"
            )
        for p in matches:
            if p.name not in seen:
                seen.add(p.name)
                selected.append(p)
    return selected
