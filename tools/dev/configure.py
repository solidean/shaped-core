"""Configure: run CMake configure for one or more presets.

Public API:
    configure(presets, ...)        -> list[StepResult]
    ensure_configured(preset, ...) -> StepResult | None

Fingerprinting lets `ensure_configured` skip the configure step when nothing
relevant changed; `configure` always reconfigures unless the fingerprint is
current (or `force` is set).
"""

from __future__ import annotations

import sys
from datetime import datetime
from pathlib import Path

from . import cmake, fingerprint, targets
from .logs import step_fields, write_sidecar
from .models import Preset, StepResult
from .process import msvc_env, run_step


def _configure_one(
    preset: Preset, *, root: Path, mirror: bool, verbose: bool
) -> StepResult:
    # Request a File API codemodel so target discovery works after configure.
    targets.write_query(preset.build_dir)
    env = msvc_env()
    result = run_step(
        cmake.configure_command(preset.configure_preset),
        label="configure",
        step=1,
        build_dir=preset.build_dir,
        cwd=root,
        env=env,
        mirror=mirror,
        verbose=verbose,
    )
    fp = ""
    if result.ok:
        fp = fingerprint.save(preset.build_dir, root)
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
) -> list[StepResult]:
    """Configure each preset. Returns one StepResult per preset that ran.

    When `force` is False and a preset's fingerprint is already current, the
    configure is skipped (no StepResult is produced for it).
    """
    results: list[StepResult] = []
    for preset in presets:
        if not force and fingerprint.is_current(preset.build_dir, root):
            print(
                f"configure: fingerprint unchanged for {preset.name!r}, skipping",
                file=sys.stderr,
            )
            continue
        results.append(_configure_one(preset, root=root, mirror=mirror, verbose=verbose))
    return results


def ensure_configured(
    preset: Preset, *, root: Path, mirror: bool = False, verbose: bool = False
) -> StepResult | None:
    """Configure `preset` only if its fingerprint is stale. Returns the result, or
    None if the configure was skipped."""
    if fingerprint.is_current(preset.build_dir, root):
        return None
    return _configure_one(preset, root=root, mirror=mirror, verbose=verbose)
