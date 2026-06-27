"""Build: build targets for one or more presets.

Public API:
    build(presets, targets, ...) -> list[StepResult]

Per preset: optionally auto-configure (cheap when fingerprint is current), then
build either the named targets or the whole project, writing a build.json
sidecar describing what ran.
"""

from __future__ import annotations

from datetime import datetime
from pathlib import Path

from . import cmake
from .configure import ensure_configured
from .logs import ninja_built_count, step_fields, write_sidecar
from .models import Preset, StepResult
from .process import env_for_preset, run_step


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
    """Build `targets` (or everything when None/empty) across all presets.

    Returns every StepResult produced, in order. A failed step does not stop the
    remaining presets/targets — the caller inspects the results for failures.
    `emsdk_path` points Emscripten presets at an emsdk install (see process.emsdk_env).
    `keep_going` passes ninja -k 0 so a build surfaces every error, not just the first.
    """
    results: list[StepResult] = []

    for preset in presets:
        # Per-preset environment: emsdk for Emscripten presets, MSVC env otherwise.
        env = env_for_preset(preset, emsdk_path)
        if auto_configure:
            cfg = ensure_configured(preset, root=root, mirror=mirror, verbose=verbose, emsdk_path=emsdk_path)
            if cfg is not None and not cfg.ok:
                results.append(cfg)
                continue  # configure failed — skip building this preset

        to_build = targets if targets else [None]
        preset_results: list[StepResult] = []
        for target in to_build:
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
