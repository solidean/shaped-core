"""CMake command construction and build-directory removal.

Pure helpers: these build the argument lists run_step executes, and remove a
build directory for the `clean` command. No project-specific knowledge.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

from . import console


def configure_command(configure_preset: str) -> list[str]:
    return ["cmake", "--preset", configure_preset]


def build_command(build_preset: str, target: str | None = None, *, keep_going: bool = False) -> list[str]:
    cmd = ["cmake", "--build", "--preset", build_preset]
    if target:
        cmd += ["--target", target]
    if keep_going:
        # Pass through to the native tool (ninja): -k 0 keeps building after a
        # failure so one run surfaces every independent error, not just the first.
        cmd += ["--", "-k", "0"]
    return cmd


def remove_build_dir(build_dir: Path, *, dry_run: bool = False) -> bool:
    """Remove a build directory tree if it exists. Returns True if it existed.

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
