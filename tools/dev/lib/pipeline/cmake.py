"""CMake command construction and build-directory removal.

Pure helpers: they build the argument lists run_step executes, and remove a build directory for `clean`.
No project-specific knowledge.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

from ..core import console


def configure_command(
    configure_preset: str, *, build_dir: Path, defines: dict[str, str] | None = None
) -> list[str]:
    # -B overrides the preset's binaryDir, and equals binaryDir for an un-overridden preset.
    # So passing it always is harmless, and lets --build-suffix / --build-dir redirect the output tree.
    # -D entries override the preset's cacheVariables, such as a pinned CMAKE_CXX_COMPILER.
    cmd = ["cmake", "--preset", configure_preset, "-B", str(build_dir)]
    for key, value in (defines or {}).items():
        cmd += ["-D", f"{key}={value}"]
    return cmd


def build_command(build_dir: Path, target: str | None = None, *, keep_going: bool = False) -> list[str]:
    # Build by directory rather than by build-preset name: the build presets are thin, name plus configurePreset only, so this is equivalent and follows an overridden build_dir.
    cmd = ["cmake", "--build", str(build_dir)]
    if target:
        cmd += ["--target", target]
    if keep_going:
        # Pass through to the native tool: ninja's -k 0 keeps building after a failure, so one run surfaces every independent error rather than just the first.
        cmd += ["--", "-k", "0"]
    return cmd


def remove_build_dir(build_dir: Path, *, dry_run: bool = False) -> bool:
    """Remove a build directory tree if it exists, returning True if it existed.

    In dry-run mode the target is only reported, never touched.
    """
    if not build_dir.exists():
        return False
    if dry_run:
        print(console.dim(f"  would remove {build_dir}"), file=sys.stderr)
        return True
    shutil.rmtree(build_dir)
    print(console.dim(f"  removed {build_dir}"), file=sys.stderr)
    return True
