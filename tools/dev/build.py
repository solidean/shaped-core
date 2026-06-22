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
from .logs import step_fields, write_sidecar
from .models import Preset, StepResult
from .process import msvc_env, run_step


def build(
    presets: list[Preset],
    targets: list[str] | None,
    *,
    root: Path,
    auto_configure: bool = True,
    mirror: bool = False,
    verbose: bool = False,
) -> list[StepResult]:
    """Build `targets` (or everything when None/empty) across all presets.

    Returns every StepResult produced, in order. A failed step does not stop the
    remaining presets/targets — the caller inspects the results for failures.
    """
    env = msvc_env()
    results: list[StepResult] = []

    for preset in presets:
        if auto_configure:
            cfg = ensure_configured(preset, root=root, mirror=mirror, verbose=verbose)
            if cfg is not None and not cfg.ok:
                results.append(cfg)
                continue  # configure failed — skip building this preset

        to_build = targets if targets else [None]
        preset_results: list[StepResult] = []
        for target in to_build:
            label = f"build-{target}" if target else "build"
            result = run_step(
                cmake.build_command(preset.name, target),
                label=label,
                step=2,
                build_dir=preset.build_dir,
                cwd=root,
                env=env,
                mirror=mirror,
                verbose=verbose,
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
                "steps": [step_fields(r, preset.build_dir) for r in preset_results],
            },
        )

    return results
