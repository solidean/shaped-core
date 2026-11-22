#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.14"
# dependencies = ["pyyaml>=6"]
# ///

"""__project__'s build & test entry point: shaped-core's driver, with this project's policy.

The driver is imported from extern/shaped-core rather than copied; this file is what is the project's own.
The preset tables, the Layout and the COMMANDS registry below are that policy, and the rest is argparse wiring.

`uv run dev.py --help` is the CLI reference.
extern/shaped-core/docs/guides/building-and-testing.md is the workflow, and extern/shaped-core/docs/dev-py-driver.md the design.
"""

from __future__ import annotations

import argparse
import atexit
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SHAPED_CORE = ROOT / "extern" / "shaped-core"

# The driver is the package `tools.dev`, so shaped-core's root goes on sys.path and this one comes off it:
# a `tools/` of our own would otherwise become a second portion of that namespace package.
sys.path[:] = [p for p in sys.path if Path(p or ".").resolve() != ROOT]
sys.path.insert(0, str(SHAPED_CORE))

from tools import dev  # noqa: E402
from tools.dev import cmd, console  # noqa: E402
from tools.dev.cmd import (  # noqa: E402
    assembly,
    benchmark,
    build,
    check,
    clean,
    compile_time,
    configure,
    coverage,
    diagnose,
    doctor,
    example,
    format,
    info,
    issues,
    lint,
    list_presets,
    list_targets,
    list_toolsets,
    pgo,
    profiling,
    run,
    ship,
    submodules,
    symbolize,
    test,
)

# The map of the CLI, in the order `dev.py --help` lists it.
# shaped-core's deps, install and test-web expect shaped-core's own files at the root, so they are not registered here.
COMMANDS = [
    configure,
    build,
    test,
    run,
    example,
    benchmark,
    format,
    lint,
    check,
    ship,
    issues,
    coverage,
    pgo,
    compile_time,
    clean,
    diagnose,
    info,
    assembly,
    symbolize,
    profiling,
    submodules,
    doctor,
    list_presets,
    list_targets,
    list_toolsets,
]

# ---------------------------------------------------------------------------
# Project policy: preset names from CMakePresets.json, which includes shaped-core's.
# ---------------------------------------------------------------------------

DEFAULT_BUILD_PRESETS: dict[str, str] = dev.DEFAULT_BUILD_PRESETS

DEFAULT_DEBUG_PRESETS: dict[str, str] = {
    "Windows": "debug-clang",
    "Linux": "debug-linux-clang",
    "Darwin": "macos-arm-llvm-debug",
}

DEFAULT_RELEASE_PRESETS: dict[str, str] = {
    "Windows": "release-clang",
    "Linux": "release-linux-clang",
    "Darwin": "macos-arm-llvm-release",
}

DEFAULT_SINGLETHREADED_PRESETS: dict[str, str] = {
    "Windows": "singlethreaded-clang",
    "Linux": "singlethreaded-linux-clang",
}

DEFAULT_SANITIZE_PRESETS: dict[str, str] = {
    "Linux": "sanitize-linux-clang",
    "Darwin": "sanitize-macos-arm-llvm",
}

DEFAULT_SANITIZE_THREAD_PRESETS: dict[str, str] = {
    "Linux": "sanitize-thread-linux-clang",
}

COVERAGE_BUILD_PRESETS: dict[str, str] = {
    "Windows": "coverage-clang",
    "Linux": "coverage-linux-clang",
    "Darwin": "coverage-macos-arm-llvm",
}

PGO_GENERATE_PRESETS: dict[str, str] = {
    "Windows": "pgo-generate-clang",
    "Linux": "pgo-generate-linux-clang",
    "Darwin": "pgo-generate-macos-arm-llvm",
}
PGO_USE_PRESETS: dict[str, str] = {
    "Windows": "pgo-use-clang",
    "Linux": "pgo-use-linux-clang",
    "Darwin": "pgo-use-macos-arm-llvm",
}
PGO_BASELINE_PRESETS: dict[str, str] = DEFAULT_RELEASE_PRESETS

DEFAULT_WEB_PRESET = "emscripten-relwithdebinfo"

# Where this project keeps its first-party files: what format, lint, crossrefs and coverage walk.
LAYOUT = dev.Layout(
    source_roots=("libs", "apps"),
    lint_roots=("libs", "apps", "docs", "tools", ".claude/skills"),
    lint_root_files=("CLAUDE.md", "README.md", "dev.py"),
    crossref_scan_dirs=("libs", "apps", "docs", "tools", ".claude"),
    crossref_root_segments=("docs/", "libs/", "apps/", "tools/", ".claude/", "extern/"),
    library_dir="libs",
)

DEFAULT_BRANCH = "main"


def build_policy() -> cmd.Policy:
    return cmd.Policy(
        default_build=DEFAULT_BUILD_PRESETS,
        default_debug=DEFAULT_DEBUG_PRESETS,
        default_release=DEFAULT_RELEASE_PRESETS,
        default_singlethreaded=DEFAULT_SINGLETHREADED_PRESETS,
        default_sanitize=DEFAULT_SANITIZE_PRESETS,
        default_sanitize_thread=DEFAULT_SANITIZE_THREAD_PRESETS,
        coverage_build=COVERAGE_BUILD_PRESETS,
        pgo_generate=PGO_GENERATE_PRESETS,
        pgo_use=PGO_USE_PRESETS,
        pgo_baseline=PGO_BASELINE_PRESETS,
        web_preset=DEFAULT_WEB_PRESET,
        layout=LAYOUT,
        default_branch=DEFAULT_BRANCH,
        # The optional build server, off unless this checkout's gitignored dev.toml `[build_server]` names a client and opts in.
        build_server=dev.build_server_from_dev_toml(ROOT),
        # shaped-core's standard gates; a gate of this project's own is added by a registry function here.
        checks=check.standard_registry,
    )


# ---------------------------------------------------------------------------
# Argument parsing & dispatch
# ---------------------------------------------------------------------------


def _force_utf8_streams() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass  # not a reconfigurable text stream


def main() -> None:
    _force_utf8_streams()

    parser = argparse.ArgumentParser(description="__project__ build & test CLI")
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    parser.add_argument("--mirror-output", action="store_true",
                        help="Stream child stdout/stderr live instead of only capturing it")
    parser.add_argument("--mirror-test-output", action="store_true",
                        help="Stream the test binaries' output live, but stay quiet through configure and build")
    parser.add_argument("--collect-logs", metavar="FILE", default=None,
                        help="On exit, bundle all captured run logs and step sidecars under build/ into a zip at FILE")
    parser.add_argument("--profile", metavar="FILE", default=None, help="Write a job profile of this run to FILE")
    parser.add_argument("--profile-type", choices=("jobs", "chrome-tracing"), default="jobs")
    parser.add_argument("--profile-lanes", choices=("global", "per-type"), default="global")
    progress_group = parser.add_mutually_exclusive_group()
    progress_group.add_argument("--progress", action="store_true", help="Force the live progress display")
    progress_group.add_argument("--no-progress", action="store_true", help="Force the terse capture-only output")
    color_group = parser.add_mutually_exclusive_group()
    color_group.add_argument("--colored", action="store_true", help="Force colored output")
    color_group.add_argument("--plain", action="store_true", help="Force plain, uncolored output")

    sub = parser.add_subparsers(dest="command", required=True)
    commands = {m.NAME: m for m in COMMANDS}
    for module in COMMANDS:
        module.add_parser(sub)

    # `test`, `run` and `example` forward unrecognized trailing tokens to the child; anywhere else one is a typo.
    args, forwarded = parser.parse_known_args()
    if forwarded and args.command not in ("test", "run", "example"):
        parser.error("unrecognized arguments: %s" % " ".join(forwarded))
    args.runner_args = forwarded
    console.configure("colored" if args.colored else "plain" if args.plain else "auto")
    dev.ui.configure("off" if args.no_progress else "on" if args.progress else "auto",
                     tail_lines=16 if args.verbose else 8)
    dev.configure_mirroring(mirror_test_output=args.mirror_test_output)
    dev.activate_vulkan_sdk()
    dev.activate_android_sdk()
    if args.profile:
        dev.profile.configure(args.profile, fmt=args.profile_type, lane_mode=args.profile_lanes,
                              argv=["dev.py", *sys.argv[1:]])

    if args.collect_logs:

        def _emit_log_archive() -> None:
            try:
                n = dev.archive_logs(ROOT / "build", Path(args.collect_logs), ROOT)
                print(f"Log archive written to {args.collect_logs} ({n} file(s))", file=sys.stderr)
            except OSError as e:
                print(f"warning: failed to write log archive {args.collect_logs}: {e}", file=sys.stderr)

        atexit.register(_emit_log_archive)

    if args.profile:

        def _emit_profile() -> None:
            try:
                dev.report.print_profile_summary(dev.profile.write(), args.profile)
            except OSError as e:
                print(f"warning: failed to write profile {args.profile}: {e}", file=sys.stderr)

        atexit.register(_emit_profile)

    # Registered last, so it runs first: the progress region has to be gone before the summaries above print.
    atexit.register(dev.ui.shutdown)

    ctx = cmd.Context(root=ROOT, policy=build_policy())
    try:
        with dev.profile.span(args.command, type="invocation", extra={"argv": sys.argv[1:]}):
            commands[args.command].run(args, ctx)
    finally:
        dev.ui.shutdown()


if __name__ == "__main__":
    main()
