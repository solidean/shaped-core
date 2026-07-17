"""Configure: run CMake configure for one or more presets.

Public API:
    configure(presets, ...)        -> list[StepResult]
    ensure_configured(preset, ...) -> StepResult | None

Fingerprinting lets `ensure_configured` skip the configure step when nothing
relevant changed; `configure` always reconfigures unless the fingerprint is
current (or `force` is set).
"""

from __future__ import annotations

import shutil
import sys
from datetime import datetime
from pathlib import Path

from . import cmake, fingerprint, prereqs
from ..core import console
from ..core.logs import step_fields, write_sidecar
from ..core.models import Preset, StepResult
from ..core.process import env_for_preset, run_step
from ..project import targets
from ..toolchain import toolset


def _publish_compile_commands(preset: Preset) -> None:
    """Copy the preset's compile_commands.json up to build/compile_commands.json.

    All presets set CMAKE_EXPORT_COMPILE_COMMANDS, so the generator emits the
    database into the per-preset build dir (build/<preset>/). clangd is pointed
    at build/compile_commands.json (see .clangd), so we publish the active
    preset's database there. This mirrors what the VSCode CMake Tools extension's
    cmake.copyCompileCommands does, but works for any configure path. With
    multiple presets the last one configured wins, which matches clangd's single
    compilation database.
    """
    src = preset.build_dir / "compile_commands.json"
    if not src.exists():
        return
    shutil.copyfile(src, preset.build_dir.parent / "compile_commands.json")


def _configure_one(
    preset: Preset, *, root: Path, mirror: bool, verbose: bool, emsdk_path: str | None = None
) -> StepResult:
    # Ensure external prerequisites (DXC, Zydis) exist before cmake sees them. Cheap once built.
    prereqs.ensure_dxc(root, preset.name)
    prereqs.ensure_zydis(root, preset.name)

    # Request a File API codemodel so target discovery works after configure.
    targets.write_query(preset.build_dir)
    env = env_for_preset(preset, emsdk_path)
    result = run_step(
        cmake.configure_command(
            preset.configure_preset,
            build_dir=preset.build_dir,
            defines=toolset.compiler_defines(preset, root),
        ),
        step_type="configure",
        build_dir=preset.build_dir,
        cwd=root,
        env=env,
        mirror=mirror,
        verbose=verbose,
    )
    fp = ""
    if result.ok:
        fp = fingerprint.save(preset.build_dir, root)
        _publish_compile_commands(preset)
    write_sidecar(
        preset.build_dir,
        "configure.json",
        {
            "timestamp": datetime.now().isoformat(timespec="seconds"),
            "configure_preset": preset.configure_preset,
            "skipped": False,
            "fingerprint": fp,
            **step_fields(result, preset.build_dir),
        },
    )
    return result


def configure(
    presets: list[Preset],
    *,
    root: Path,
    force: bool = False,
    mirror: bool = False,
    verbose: bool = False,
    emsdk_path: str | None = None,
) -> list[StepResult]:
    """Configure each preset. Returns one StepResult per preset that ran.

    When `force` is False and a preset's fingerprint is already current, the
    configure is skipped (no StepResult is produced for it). `emsdk_path` points
    Emscripten presets at an emsdk install (see process.emsdk_env).
    """
    results: list[StepResult] = []
    for preset in presets:
        if not force and fingerprint.is_current(preset.build_dir, root):
            print(
                console.dim(f"configure: fingerprint unchanged for {preset.name!r}, skipping"),
                file=sys.stderr,
            )
            continue
        results.append(_configure_one(preset, root=root, mirror=mirror, verbose=verbose, emsdk_path=emsdk_path))
    return results


def ensure_configured(
    preset: Preset, *, root: Path, mirror: bool = False, verbose: bool = False,
    emsdk_path: str | None = None,
) -> StepResult | None:
    """Configure `preset` only if its fingerprint is stale. Returns the result, or
    None if the configure was skipped."""
    if fingerprint.is_current(preset.build_dir, root):
        return None
    return _configure_one(preset, root=root, mirror=mirror, verbose=verbose, emsdk_path=emsdk_path)
