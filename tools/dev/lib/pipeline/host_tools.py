"""Host tools: the programs a build runs on the build machine, built for it rather than for the preset's target.

A cross preset compiles every target for its own machine, the tree's own `sgl` included, and the build machine cannot run that one.
Emscripten is the one that matters today: every WebAssembly preset builds the SGL shader packages of the tier-1 tests.
So a shader package whose SGL entries the compiler has to read would have nothing to read them with.
This builds `sgl` in the platform's default native preset and hands its path to the cross build as SC_SGL_TOOL.
`SC_HOST_TOOLSET` pins that preset's toolset, as `--toolset` does for a native one; the cross build's own `--toolset` names its cross compiler instead.
A cross CI runner sets it, since the image's default compiler is not one the tree builds with.

Every cross build refreshes it first, not only the first configure.
The package generator depends on the tool's file, so a stale one would regenerate nothing and describe shaders the way an older compiler did.

Public API:
    host_preset(root)                      -> Preset
    ensure_host_sgl(root, ...)             -> Path | StepResult
"""

from __future__ import annotations

import os
import platform
from pathlib import Path

from . import cmake
from .configure import ensure_configured
from ..core.models import Preset, StepResult
from ..core.process import env_for_preset, run_step
from ..project import targets
from ..project.presets import DEFAULT_BUILD_PRESETS, resolve_presets
from ..toolchain.toolset import ToolsetError, apply_overrides

SGL_TARGET = "sgl"


def host_preset(root: Path) -> Preset:
    """The native preset host tools are built in: the platform's default one, at `SC_HOST_TOOLSET` when that is set."""
    preset = resolve_presets(root, [DEFAULT_BUILD_PRESETS[platform.system()]])[0]
    toolset = os.environ.get("SC_HOST_TOOLSET") or None
    if toolset is None:
        return preset
    try:
        return apply_overrides([preset], root=root, toolset=toolset)[0]
    except ToolsetError as e:
        raise ToolsetError(f"SC_HOST_TOOLSET={toolset} for the host preset: {e}") from None


def ensure_host_sgl(root: Path, *, mirror: bool = False, verbose: bool = False) -> Path | StepResult:
    """A runnable `sgl`, built in the host preset: its path, or the step that failed to produce it."""
    host = host_preset(root)
    configured = ensure_configured(host, root=root, mirror=mirror, verbose=verbose)
    if configured is not None and not configured.ok:
        return configured

    built = run_step(
        cmake.build_command(host.build_dir, SGL_TARGET),
        step_type="build",
        name=f"{SGL_TARGET}-host",  # the one built for the cross build, not the cross build's own
        build_dir=host.build_dir,
        cwd=root,
        env=env_for_preset(host, None),
        mirror=mirror,
        verbose=verbose,
    )
    if not built.ok:
        return built

    for target in targets.discover_targets(host.build_dir, host.build_type):
        if target.name == SGL_TARGET and target.artifact is not None:
            return target.artifact
    raise RuntimeError(f"the host preset {host.name!r} built `{SGL_TARGET}` but its artifact was not found")
