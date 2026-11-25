"""Host tools: the programs a build runs on the build machine, built for it rather than for the preset's target.

A cross preset compiles every target for its own machine, the tree's own `sgl` included, and the build machine cannot run that one.
Emscripten is the one that matters today: every WebAssembly preset builds the SGL shader packages of the tier-1 tests.
So a shader package whose SGL entries the compiler has to read would have nothing to read them with.
This builds `sgl` in the platform's default native preset, deploys a copy of it to build/host-tools/, and hands the copy's path to the cross build as SC_SGL_TOOL.
A copy, because a cross build runs it outside every dependency graph the native build knows of, the build server's background rebuilds included.
Run in place, it would hold the native build's own output open, and on Windows the next native link of `sgl` could not replace it.
`SC_HOST_TOOLSET` pins that preset's toolset, as `--toolset` does for a native one; the cross build's own `--toolset` names its cross compiler instead.
A cross CI runner sets it, since the image's default compiler is not one the tree builds with.

Every cross build refreshes it first, not only the first configure.
The package generator depends on the tool's file, so a stale one would regenerate nothing and describe shaders the way an older compiler did.

Public API:
    host_preset(root)                      -> Preset
    ensure_host_sgl(root, ...)             -> Path | StepResult
    deployed_sgl(root)                     -> Path
    is_tool_current(build_dir, root)       -> bool
"""

from __future__ import annotations

import filecmp
import os
import platform
import shutil
import time
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


def deployed_sgl(root: Path) -> Path:
    """Where the host `sgl` a cross build runs is deployed: one per checkout, whatever the cross preset."""
    return root / "build" / "host-tools" / ("sgl.exe" if platform.system() == "Windows" else "sgl")


def is_tool_current(build_dir: Path, root: Path) -> bool:
    """Whether a configured cross build already runs the deployed `sgl`; one that runs any other must reconfigure."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return True
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("SC_SGL_TOOL:"):
            return line.partition("=")[2].strip() == deployed_sgl(root).as_posix()
    return False


def _deploy(built: Path, root: Path) -> Path:
    """Copies `built` over the deployed `sgl` where it differs, and returns the deployed path.

    The copy lands beside it first and then takes its name, so a run never starts half a file.
    A deployed `sgl` that is running cannot be replaced on Windows but can be renamed, so it is moved aside first;
    what was moved aside goes once nothing runs it any more.
    """
    target = deployed_sgl(root)
    if target.is_file() and filecmp.cmp(built, target, shallow=False):
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    fresh = target.with_name(target.name + ".new")
    shutil.copy2(built, fresh)
    try:
        os.replace(fresh, target)
    except PermissionError:
        target.rename(target.with_name(f"{target.name}.old-{time.time_ns()}"))
        os.replace(fresh, target)
    for old in target.parent.glob(f"{target.name}.old-*"):
        try:
            old.unlink()
        except OSError:
            pass  # still running; the next deploy tries again
    return target


def ensure_host_sgl(root: Path, *, mirror: bool = False, verbose: bool = False) -> Path | StepResult:
    """A runnable `sgl`, built in the host preset and deployed: its deployed path, or the step that failed to produce it."""
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
            return _deploy(target.artifact, root)
    raise RuntimeError(f"the host preset {host.name!r} built `{SGL_TARGET}` but its artifact was not found")
