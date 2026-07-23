#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""
Build & test CLI for shaped-core.

Usage:
    uv run dev.py configure                 Configure the CMake project
    uv run dev.py build [--target T]        Build (optionally specific targets)
    uv run dev.py test [--target T] [NAME]  Run tests (optionally a binary / test name)
    uv run dev.py test-web [LIBRARY]        Open the browser test runner (Emscripten; all libs, or one)
    uv run dev.py format [--dirty-only]     Format libs/ sources with clang-format
    uv run dev.py lint clang-tidy           Run the clang-tidy whitelist gates (must be zero to commit)
    uv run dev.py check [NAME...] [--fix]   Run pre-commit checks (format, lint, crossrefs, test)
    uv run dev.py coverage run [NAME]       Collect LLVM test coverage (run/merge/report)
    uv run dev.py pgo run                    Profile-guided optimization (instrument/train/optimize/measure)
    uv run dev.py clean [--all]             Remove build artifacts
    uv run dev.py info build-flags TARGET   Show resolved compile/link flags (or compile-command FILE)
    uv run dev.py assembly search PATTERN   Search symbols in built objects (or show SYMBOL to disassemble)
    uv run dev.py diagnose clangd FILE      Show clangd diagnostics for a source file
    uv run dev.py doctor                    Sanity-check the toolchain
    uv run dev.py list-presets              List available build presets
    uv run dev.py list-targets             List discovered targets
    uv run dev.py list-toolsets             List installed compiler toolsets (for --toolset)

Presets and targets accept comma-lists, repeated flags, and shell-style
wildcards, and operate on as many as you select:
    --preset debug-clang,release-clang
    --preset "x64-linux-*" --preset debug-clang
    --target "*-test"

The default preset is chosen by platform but can be overridden with --preset.
'build' and 'test' auto-configure when cmake inputs or the source listing change
(fingerprinted); pass --no-configure to skip.

dev.py is quiet by default: child stdout/stderr is captured to
build/<preset>/run-logs/run-log-<name>.{stdout,stderr}.txt rather than
mirrored to the terminal — pass --mirror-output to stream all of it live, or
--mirror-test-output to stream only the test binaries (quiet build, loud test).
Each command also writes a machine-readable sidecar (configure.json /
build.json / test.json) next to the build directory.

Output is colored (green/orange/red) when stdout and stderr are both a terminal,
and plain when either is piped (e.g. run by an agent). Force it either way with
--colored / --plain; in auto mode NO_COLOR / FORCE_COLOR are also honored.

This file is project policy + wiring: the preset tables below, plus the argparse
loop and dispatch. The reusable, project-agnostic machinery lives in tools/dev/
(facade in tools/dev/__init__.py); each command's implementation lives in
tools/dev/cmd/. See docs/dev-py-driver.md for the design and how to extend it.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))

from tools import dev  # noqa: E402
from tools.dev import cmd, console  # noqa: E402
from tools.dev.cmd import (  # noqa: E402
    assembly,
    build,
    check,
    clean,
    configure,
    coverage,
    diagnose,
    doctor,
    format,
    info,
    lint,
    list_presets,
    list_targets,
    list_toolsets,
    pgo,
    profiling,
    test,
    test_web,
)

# The command registry: one module per command, in the order they appear in
# `dev.py --help`. This is the map of the CLI — click through to a command's
# module for its flags and logic. Adding a command is a new module in
# tools/dev/cmd/ plus one line here.
COMMANDS = [
    configure,
    build,
    test,
    test_web,
    format,
    lint,
    check,
    coverage,
    pgo,
    clean,
    diagnose,
    info,
    assembly,
    profiling,
    doctor,
    list_presets,
    list_targets,
    list_toolsets,
]

# ---------------------------------------------------------------------------
# Project policy
#
# Which presets each command reaches for, keyed by platform.system(). This is the
# one place a downstream fork edits to point the driver at its own presets; the rest is
# generic wiring. The tables are bundled into a cmd.Policy and handed to each
# command via the Context (see tools/dev/cmd/context.py).
# ---------------------------------------------------------------------------

# Default build preset per platform. Override with --preset.
DEFAULT_BUILD_PRESETS: dict[str, str] = {
    "Windows": "relwithdebinfo-clang",
    "Linux": "relwithdebinfo-linux-clang",
    "Darwin": "macos-arm-llvm-relwithdebinfo",
}

# Debug sibling of each default preset. Run by the `test` check alongside the
# others: -O0 plus mimalloc's debug heap (MI_DEBUG) catch bugs the optimized
# presets miss.
DEFAULT_DEBUG_PRESETS: dict[str, str] = {
    "Windows": "debug-clang",
    "Linux": "debug-linux-clang",
    "Darwin": "macos-arm-llvm-debug",
}

# Release sibling of each default preset, run alongside it by the `test` check so
# precommit exercises both CC_ASSERT on (debug/relwithdebinfo) and off (release).
DEFAULT_RELEASE_PRESETS: dict[str, str] = {
    "Windows": "release-clang",
    "Linux": "release-linux-clang",
    "Darwin": "macos-arm-llvm-release",
}

# Single-threaded sibling of each default preset (SC_THREADS=OFF -> CC_HAS_THREADS 0), run by the `test`
# check so precommit exercises both threading modes rather than only the threaded one. RelWithDebInfo, so
# it also carries CC_ASSERT. Without it this mode is only reachable via a wasm build, which is how it came
# to be under-exercised in the first place. Deliberately one preset, not a matrix: it is a compile-time
# axis, and the check tail is already the slow part.
DEFAULT_SINGLETHREADED_PRESETS: dict[str, str] = {
    "Windows": "singlethreaded-clang",
    "Linux": "singlethreaded-linux-clang",
    "Darwin": "singlethreaded-macos-arm-llvm",
}

# Sanitizer (ASan+UBSan) preset per platform, run by the `test` check as an extra
# defensive pass. Windows is intentionally absent: clang-cl's ASan is broken with
# C++ exceptions (any throw/catch faults during EH dispatch — a toolchain bug, not
# ours), and nexus catches test exceptions, so it can never be green there. The
# sanitize-clang preset still exists for manual non-throwing runs; see
# docs/guides/building-and-testing.md.
DEFAULT_SANITIZE_PRESETS: dict[str, str] = {
    "Linux": "sanitize-linux-clang",
    "Darwin": "sanitize-macos-arm-llvm",
}

# Default coverage preset per platform (instrumented Debug build; SC_COVERAGE ON).
# `coverage` uses these instead of DEFAULT_BUILD_PRESETS when no --preset is given.
COVERAGE_BUILD_PRESETS: dict[str, str] = {
    "Windows": "coverage-clang",
    "Linux": "coverage-linux-clang",
    "Darwin": "coverage-macos-arm-llvm",
}

# PGO presets per platform: the instrumented (-fprofile-generate) and optimized
# (-fprofile-use) Release builds, plus the clean Release baseline the speedup is
# measured against. `pgo` uses these when no --preset is given.
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
PGO_BASELINE_PRESETS: dict[str, str] = {
    "Windows": "release-clang",
    "Linux": "release-linux-clang",
    "Darwin": "macos-arm-llvm-release",
}

# Default preset for `test-web` (the browser runner is Emscripten-only, regardless of host platform).
DEFAULT_WEB_PRESET = "emscripten-relwithdebinfo"


def build_policy() -> cmd.Policy:
    """Bundle the preset tables above into the Policy commands read through the Context."""
    return cmd.Policy(
        default_build=DEFAULT_BUILD_PRESETS,
        default_debug=DEFAULT_DEBUG_PRESETS,
        default_release=DEFAULT_RELEASE_PRESETS,
        default_singlethreaded=DEFAULT_SINGLETHREADED_PRESETS,
        default_sanitize=DEFAULT_SANITIZE_PRESETS,
        coverage_build=COVERAGE_BUILD_PRESETS,
        pgo_generate=PGO_GENERATE_PRESETS,
        pgo_use=PGO_USE_PRESETS,
        pgo_baseline=PGO_BASELINE_PRESETS,
        web_preset=DEFAULT_WEB_PRESET,
    )


# ---------------------------------------------------------------------------
# Argument parsing & dispatch
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="shaped-core build & test CLI",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--verbose", "-v", action="store_true", help="Verbose output")
    parser.add_argument("--mirror-output", action="store_true",
                        help="Stream child stdout/stderr live instead of only capturing it")
    parser.add_argument("--mirror-test-output", action="store_true",
                        help="Stream the test binaries' stdout/stderr live, but stay quiet through "
                             "configure and build (the common case; --mirror-output does both)")
    parser.add_argument("--collect-logs", metavar="FILE", default=None,
                        help="On exit (pass or fail), bundle all captured run logs and step sidecars "
                             "under build/ into a zip at FILE — last-resort raw diagnostics for CI.")
    color_group = parser.add_mutually_exclusive_group()
    color_group.add_argument("--colored", action="store_true",
                             help="Force colored output (default: auto-detect by terminal)")
    color_group.add_argument("--plain", action="store_true",
                             help="Force plain, uncolored output")

    sub = parser.add_subparsers(dest="command", required=True)
    commands = {m.NAME: m for m in COMMANDS}
    for module in COMMANDS:
        module.add_parser(sub)

    # parse_known_args (not parse_args) so `test` can forward unrecognized trailing tokens
    # verbatim to the test binary (e.g. `-c <section>` for nexus section scoping) while dev.py's
    # own options — crucially --preset — are still parsed no matter where they sit relative to the
    # test-name positional. A REMAINDER positional used to swallow everything after the name,
    # silently eating a trailing `--preset` and running the default preset instead (a real hazard:
    # a `--preset release-clang` verify could actually run relwithdebinfo). For every other command
    # unknown args stay a hard error, mirroring argparse's strict default so typos fail loudly.
    args, forwarded = parser.parse_known_args()
    if forwarded and args.command != "test":
        parser.error("unrecognized arguments: %s" % " ".join(forwarded))
    args.runner_args = forwarded
    console.configure("colored" if args.colored else "plain" if args.plain else "auto")
    dev.configure_mirroring(mirror_test_output=args.mirror_test_output)

    # Capture logs on the way out regardless of how the command exits (including
    # sys.exit on a failed build/test) — atexit fires on SystemExit too.
    if args.collect_logs:
        import atexit

        def _emit_log_archive() -> None:
            try:
                n = dev.archive_logs(ROOT / "build", Path(args.collect_logs), ROOT)
                print(f"Log archive written to {args.collect_logs} ({n} file(s))", file=sys.stderr)
            except OSError as e:
                print(f"warning: failed to write log archive {args.collect_logs}: {e}", file=sys.stderr)

        atexit.register(_emit_log_archive)

    ctx = cmd.Context(root=ROOT, policy=build_policy())
    commands[args.command].run(args, ctx)


if __name__ == "__main__":
    main()
