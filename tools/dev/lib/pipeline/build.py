"""Build: build targets for one or more presets.

Per preset: optionally auto-configure, which is cheap when the fingerprint is current, then build either the named targets or the whole project.
A build.json sidecar describing what ran is written alongside.

Public API:
    build(presets, targets, ...) -> list[StepResult]
"""

from __future__ import annotations

import time
from datetime import datetime
from pathlib import Path

from . import cmake, diagjobs
from .configure import ensure_configured_all
from ..core import profile
from ..core.logs import ninja_built_count, step_fields, write_sidecar
from ..core.models import Preset, StepResult
from ..core.process import env_for_preset, run_step


def _build_extra(result: StepResult) -> str:
    """Summary suffix for a build step: how many files ninja (re)built."""
    n = ninja_built_count(result.stdout_log)
    return f" ({n} file{'s' if n != 1 else ''})" if n else " (up to date)"


def build(
    presets: list[Preset],
    targets: list[str] | None,
    *,
    root: Path,
    auto_configure: bool = True,
    mirror: bool = False,
    verbose: bool = False,
    emsdk_path: str | None = None,
    keep_going: bool = False,
) -> list[StepResult]:
    """Build `targets`, or everything when None or empty, across all presets.

    Returns every StepResult produced, in order.
    A failed step does not stop the remaining presets or targets, so the caller inspects the results for failures.
    `emsdk_path` points Emscripten presets at an emsdk install (see process.emsdk_env), and `keep_going` passes ninja -k 0.
    """
    results: list[StepResult] = []

    # All presets configure first, together, rather than each one immediately before its own build.
    # The builds still run one preset at a time, since each already saturates the machine, but a configure does not.
    # Serializing four of them behind three builds was the single largest idle stretch in a cold run.
    failed_configure: set[str] = set()
    if auto_configure:
        for preset, cfg in ensure_configured_all(presets, root=root, mirror=mirror, verbose=verbose,
                                                 emsdk_path=emsdk_path):
            if not cfg.ok:
                results.append(cfg)
                failed_configure.add(preset.name)

    for preset in presets:
        if preset.name in failed_configure:
            continue  # configure failed — skip building this preset
        # Per-preset environment: emsdk for Emscripten presets, MSVC env otherwise.
        env = env_for_preset(preset, emsdk_path)

        to_build = targets if targets else [None]
        preset_results: list[StepResult] = []
        for target in to_build:
            # Marked before the step because the compile sidecars accumulate across builds, and only the ones this step rewrote are ours.
            build_mark = diagjobs.mark(preset.build_dir) if profile.enabled() else None
            result = run_step(
                cmake.build_command(preset.build_dir, target, keep_going=keep_going),
                step_type="build",
                name=target or "all",
                build_dir=preset.build_dir,
                cwd=root,
                env=env,
                mirror=mirror,
                verbose=verbose,
                summary_extra=_build_extra,
            )
            if build_mark is not None:
                profile.add_jobs(diagjobs.harvest(preset.build_dir, build_mark, ended_at=time.time()))
            preset_results.append(result)
            if not result.ok:
                break  # stop this preset on first build failure

        results.extend(preset_results)
        write_sidecar(
            preset.build_dir,
            "build.json",
            {
                "timestamp": datetime.now().isoformat(timespec="seconds"),
                "targets": targets if targets else "all",
                "steps": [
                    {**step_fields(r, preset.build_dir), "built": ninja_built_count(r.stdout_log)}
                    for r in preset_results
                ],
            },
        )

    return results
