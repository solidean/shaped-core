"""Reusable developer tooling for CMake-based projects.

Plain functions over plain data — orchestration (argument parsing, command
dispatch, project-specific policy) lives in the project's dev.py; this package
only provides helpers. Everything is collection-oriented: configure/build/test
operate on lists of presets and targets.
"""

from __future__ import annotations

from .lib.core import console, report
from .lib.core.archive import archive_diag, archive_logs
from .lib.core.logs import merge_junit, ninja_built_count
from .lib.core.models import CompileGroup, Preset, StepResult, Target, TargetFlags, TestSummary
from .lib.core.process import configure_mirroring, emsdk_env, find_emsdk_root, response_file, run_step
from .lib.pipeline.build import build
from .lib.pipeline.cmake import remove_build_dir
from .lib.pipeline.configure import configure, ensure_configured
from .lib.pipeline.eligibility import select_eligible_binaries
from .lib.pipeline.test import test
from .lib.project.compdb import find_entry, load_entries, suggest_files
from .lib.project.flags import extract_flags
from .lib.project.presets import PresetError, load_presets, resolve_cache_variable, resolve_presets
from .lib.project.targets import (
    NotConfiguredError,
    discover_targets,
    executables,
    load_target_models,
    select_test_binaries,
    write_query,
)
from .lib.quality.checks import Check, list_checks, run_checks
from .lib.quality.crossrefs import CrossRefResult, check_crossrefs
from .lib.quality.format import (
    FormatResult,
    FormatSetupError,
    clang_format_version,
    discover_files,
    find_clang_format,
    format_sources,
    required_major,
    run_format,
    source_roots,
    violating_files,
)
from .lib.toolchain import clangd
from .lib.toolchain.doctor import doctor
from .lib.toolchain.llvm_tools import resolve_tool
from .lib.toolchain.toolset import ToolsetError, apply_overrides, list_toolsets, toolset_hint
from .lib.perf.coverage import (
    CoverageToolError,
    coverage_merge,
    coverage_report,
    coverage_run,
    find_tool,
)
from .lib.perf.perf import run_and_collect as perf_run_and_collect
from .lib.perf.pgo import (
    PgoError,
    pgo_instrument,
    pgo_measure,
    pgo_optimize,
    pgo_run,
    pgo_train,
    profile_path,
)

__all__ = [
    "archive_diag",
    "archive_logs",
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
    "resolve_tool",
    "PgoError",
    "pgo_instrument",
    "pgo_train",
    "pgo_optimize",
    "pgo_measure",
    "pgo_run",
    "profile_path",
    "perf_run_and_collect",
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
    "load_entries",
    "find_entry",
    "suggest_files",
    "extract_flags",
    "Preset",
    "Target",
    "CompileGroup",
    "TargetFlags",
    "StepResult",
    "TestSummary",
    "load_target_models",
    "load_presets",
    "resolve_presets",
    "apply_overrides",
    "list_toolsets",
    "toolset_hint",
    "ToolsetError",
    "configure_mirroring",
    "emsdk_env",
    "find_emsdk_root",
    "response_file",
    "run_step",
    "source_roots",
    "PresetError",
    "discover_targets",
    "executables",
    "select_test_binaries",
    "select_eligible_binaries",
    "write_query",
    "NotConfiguredError",
    "test",
]
