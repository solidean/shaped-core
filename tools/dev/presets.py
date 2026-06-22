"""Load and resolve presets from CMakePresets.json.

Public API:
    load_presets(root)            -> list[Preset]   (all non-hidden build presets)
    resolve_presets(root, specs)  -> list[Preset]   (filtered by comma/glob/repeat)

Hidden configure presets are inheritance scaffolding and are never returned; a
build preset's CMAKE_BUILD_TYPE is resolved by walking its configure preset's
`inherits` chain.
"""

from __future__ import annotations

import fnmatch
import json
from collections.abc import Sequence
from pathlib import Path

from .models import Preset


class PresetError(Exception):
    """Raised when a preset spec cannot be resolved or the file is malformed."""


def _read_presets_file(root: Path) -> dict:
    path = root / "CMakePresets.json"
    if not path.is_file():
        raise PresetError(f"CMakePresets.json not found at {path}")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


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
