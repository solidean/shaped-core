"""Configure: run CMake configure for one or more presets.

`configure` always reconfigures unless the fingerprint is current or `force` is set, and `ensure_configured` skips when nothing relevant changed.
`ensure_configured_all` is the multi-preset form, and the one that matters for wall clock.
A cold `check` configures four presets, each ~20 s of SDL3 feature probes, and they have no reason to wait for each other.

Public API:
    configure(presets, ...)            -> list[StepResult]
    ensure_configured(preset, ...)     -> StepResult | None
    ensure_configured_all(presets, ...) -> list[StepResult]
"""

from __future__ import annotations

import shutil
import sys
from concurrent.futures import ThreadPoolExecutor
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

    Every preset sets CMAKE_EXPORT_COMPILE_COMMANDS, so the generator emits the database into build/<preset>/.
    .clangd points clangd at build/compile_commands.json, and publishing the active preset's database there is what connects the two.
    With several presets the last one configured wins, which matches clangd's single compilation database.
    """
    src = preset.build_dir / "compile_commands.json"
    if not src.exists():
        return
    shutil.copyfile(src, preset.build_dir.parent / "compile_commands.json")


def _ensure_prereqs(root: Path, preset: Preset) -> None:
    """Fetch the external prerequisites this preset needs; prereqs.py carries the policy.

    Kept out of `_configure_one` so the concurrent path can run it once per preset up front:
    four threads racing the same fetch script into the same extern/ directory would corrupt the install.
    """
    prereqs.ensure_dxc(root, preset.name)
    prereqs.ensure_zydis(root, preset.name)
    prereqs.ensure_sdl3(root, preset.name)
    prereqs.ensure_sqlite(root, preset.name)


def _configure_one(
    preset: Preset, *, root: Path, mirror: bool, verbose: bool, emsdk_path: str | None = None,
    prereqs_done: bool = False, publish: bool = True, concurrent: bool = False,
) -> StepResult:
    if not prereqs_done:
        _ensure_prereqs(root, preset)

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
        name=preset.name if concurrent else None,  # concurrent banners interleave, so each must say which preset it is
        cwd=root,
        env=env,
        mirror=mirror,
        verbose=verbose,
        profile_origin="external" if concurrent else "driver",
    )
    fp = ""
    if result.ok:
        fp = fingerprint.save(preset.build_dir, root)
        if publish:
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
    """Configure each preset, returning one StepResult per preset that ran.

    With `force` False, a preset whose fingerprint is already current is skipped and produces no StepResult.
    `emsdk_path` points Emscripten presets at an emsdk install (see process.emsdk_env).
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
    """Configure `preset` only if its fingerprint is stale.

    Returns the result, or None when the configure was skipped.
    """
    if fingerprint.is_current(preset.build_dir, root):
        return None
    return _configure_one(preset, root=root, mirror=mirror, verbose=verbose, emsdk_path=emsdk_path)


def ensure_configured_all(
    presets: list[Preset], *, root: Path, mirror: bool = False, verbose: bool = False,
    emsdk_path: str | None = None,
) -> list[tuple[Preset, StepResult]]:
    """Configure every stale preset in `presets`, concurrently, returning (preset, result) for each one that ran.

    Presets that are already current cost nothing and are simply absent from the result.
    The pairing is what the caller needs — a failed configure means "do not build THIS preset" — and a bare StepResult does not say which one it was.
    The configures are independent — separate build dirs, separate CMakeFiles/CMakeTmp for try_compile — and CMake is single-threaded, so running them at once is close to free next to the serial cost.
    Two things are deliberately NOT concurrent: the prerequisite fetches, which share extern/, and publishing build/compile_commands.json, which is one shared file and must land deterministically.
    """
    stale = [p for p in presets if not fingerprint.is_current(p.build_dir, root)]
    if not stale:
        return []

    for preset in stale:
        _ensure_prereqs(root, preset)

    if len(stale) == 1:
        return [(stale[0], _configure_one(stale[0], root=root, mirror=mirror, verbose=verbose,
                                          emsdk_path=emsdk_path, prereqs_done=True))]

    print(console.dim(f"configure: {len(stale)} presets in parallel"), file=sys.stderr)
    with ThreadPoolExecutor(max_workers=len(stale)) as pool:
        results = list(zip(stale, pool.map(
            lambda p: _configure_one(p, root=root, mirror=mirror, verbose=verbose, emsdk_path=emsdk_path,
                                     prereqs_done=True, publish=False, concurrent=True),
            stale,
        )))

    # clangd reads a single database, so the primary preset owns build/compile_commands.json.
    # Serial configure gave it to whoever went last; pinning it to presets[0] is what keeps that stable now that order is not defined.
    primary = next((p for p in presets if p.build_dir.joinpath("compile_commands.json").exists()), None)
    if primary is not None:
        _publish_compile_commands(primary)
    return results
