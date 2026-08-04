#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///

"""shaped-core's build & test entry point: project policy plus CLI wiring.

The preset tables below are the policy — which presets each command reaches for, on each platform.
Those tables and the COMMANDS registry are what a downstream fork edits; the rest of this file is argparse wiring and dispatch.
The reusable machinery lives in tools/dev/ (flat facade in tools/dev/__init__.py), and each command's implementation under tools/dev/cmd/.

`uv run dev.py --help` is the CLI reference.
docs/guides/building-and-testing.md is the workflow, and docs/dev-py-driver.md the design and how to extend the driver.
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
    run,
    test,
    test_web,
)

# The map of the CLI: one module per command, in the order `dev.py --help` lists them.
# A command's flags and logic live in its own module under tools/dev/cmd/.
COMMANDS = [
    configure,
    build,
    test,
    test_web,
    run,
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
# Project policy — the tables below, bundled into a cmd.Policy and reaching commands through the Context (tools/dev/cmd/context.py).
# ---------------------------------------------------------------------------

# Default build preset per platform, overridable with --preset.
DEFAULT_BUILD_PRESETS: dict[str, str] = {
    "Windows": "relwithdebinfo-clang",
    "Linux": "relwithdebinfo-linux-clang",
    "Darwin": "macos-arm-llvm-relwithdebinfo",
}

# Debug sibling of each default preset, run by the `test` check alongside the others.
DEFAULT_DEBUG_PRESETS: dict[str, str] = {
    "Windows": "debug-clang",
    "Linux": "debug-linux-clang",
    "Darwin": "macos-arm-llvm-debug",
}

# Release sibling of each default preset — the `test` check's CC_ASSERT-off leg.
DEFAULT_RELEASE_PRESETS: dict[str, str] = {
    "Windows": "release-clang",
    "Linux": "release-linux-clang",
    "Darwin": "macos-arm-llvm-release",
}

# Single-threaded sibling of each default preset (SC_THREADS=OFF -> CC_HAS_THREADS 0), run by the `test` check.
# Deliberately one preset rather than a matrix: threading is a compile-time axis, and the check's test tail is already the slow part.
DEFAULT_SINGLETHREADED_PRESETS: dict[str, str] = {
    "Windows": "singlethreaded-clang",
    "Linux": "singlethreaded-linux-clang",
    "Darwin": "singlethreaded-macos-arm-llvm",
}

# Sanitizer (ASan+UBSan) preset per platform, run by the `test` check.
# Windows is absent deliberately, and the Sanitizers section of docs/guides/building-and-testing.md says why.
DEFAULT_SANITIZE_PRESETS: dict[str, str] = {
    "Linux": "sanitize-linux-clang",
    "Darwin": "sanitize-macos-arm-llvm",
}

# Default coverage preset per platform (RelWithDebInfo, SC_COVERAGE ON).
# `coverage` uses these instead of DEFAULT_BUILD_PRESETS when no --preset is given.
COVERAGE_BUILD_PRESETS: dict[str, str] = {
    "Windows": "coverage-clang",
    "Linux": "coverage-linux-clang",
    "Darwin": "coverage-macos-arm-llvm",
}

# PGO presets per platform: the instrumented (-fprofile-generate) and optimized (-fprofile-use) Release builds.
# PGO_BASELINE is the clean Release the speedup is measured against.
# `pgo` uses all three when no --preset is given.
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

def _force_utf8_streams() -> None:
    """Make dev.py's own stdout/stderr UTF-8, however they are attached.

    Python's streams are the one link in this process that is not already UTF-8: redirected on Windows they fall back to the locale encoding.
    `errors="replace"` is the second belt, so a stream that cannot be reconfigured at all degrades to `?` rather than throwing.
    docs/dev-py-driver.md says why a mid-mirror encoding failure mattered far more than the garbled character.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError, OSError):
            pass # not a reconfigurable text stream (a redirect in a test harness, a closed stream)


def main() -> None:
    _force_utf8_streams()

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

    # parse_known_args (not parse_args) so `test` and `run` can forward unrecognized trailing tokens verbatim to the child process.
    # dev.py's own options must still parse wherever they sit relative to the positional.
    # --preset above all: a catch-all positional would silently drop it and run the default preset instead.
    # Everywhere else an unknown argument stays a hard error, so a typo fails loudly.
    args, forwarded = parser.parse_known_args()
    if forwarded and args.command not in ("test", "run"):
        parser.error("unrecognized arguments: %s" % " ".join(forwarded))
    args.runner_args = forwarded
    console.configure("colored" if args.colored else "plain" if args.plain else "auto")
    dev.configure_mirroring(mirror_test_output=args.mirror_test_output)

    # atexit fires on SystemExit too, so the archive is written however the command exits — a failed build or test included.
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
