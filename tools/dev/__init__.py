"""Reusable developer tooling for CMake-based projects.

Plain functions over plain data — orchestration (argument parsing, command
dispatch, project-specific policy) lives in the project's dev.py; this package
only provides helpers. Everything is collection-oriented: configure/build/test
operate on lists of presets and targets.
"""

from __future__ import annotations

from . import clangd, console, report
from .build import build
from .checks import Check, list_checks, run_checks
from .cmake import remove_build_dir
from .configure import configure, ensure_configured
from .coverage import (
    CoverageToolError,
    coverage_merge,
    coverage_report,
    coverage_run,
    find_tool,
)
from .crossrefs import CrossRefResult, check_crossrefs
from .doctor import doctor
from .format import (
    FormatResult,
    FormatSetupError,
    clang_format_version,
    discover_files,
    find_clang_format,
    format_sources,
    required_major,
    run_format,
    violating_files,
)
from .logs import merge_junit, ninja_built_count
from .models import Preset, StepResult, Target, TestSummary
from .presets import PresetError, load_presets, resolve_presets
from .process import emsdk_env, find_emsdk_root
from .targets import (
    NotConfiguredError,
    discover_targets,
    executables,
    select_test_binaries,
    write_query,
)
from .test import test

__all__ = [
    "build",
    "clangd",
    "console",
    "report",
    "Check",
    "list_checks",
    "run_checks",
    "configure",
    "ensure_configured",
    "coverage_run",
    "coverage_report",
    "coverage_merge",
    "CoverageToolError",
    "find_tool",
    "CrossRefResult",
    "check_crossrefs",
    "doctor",
    "FormatResult",
    "FormatSetupError",
    "clang_format_version",
    "discover_files",
    "find_clang_format",
    "format_sources",
    "required_major",
    "run_format",
    "violating_files",
    "merge_junit",
    "ninja_built_count",
    "remove_build_dir",
    "Preset",
    "Target",
    "StepResult",
    "TestSummary",
    "load_presets",
    "resolve_presets",
    "emsdk_env",
    "find_emsdk_root",
    "PresetError",
    "discover_targets",
    "executables",
    "select_test_binaries",
    "write_query",
    "NotConfiguredError",
    "test",
]
