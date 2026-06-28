"""Shared argparse fragments reused across command subparsers.

Each command module owns its own subparser (see cmd/__init__.py); these helpers
keep the common flags — preset selection, the build-dir overrides, and the emsdk
path — defined once instead of copied per command.
"""

from __future__ import annotations

import argparse


def preset(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--preset", action="append",
        help="Build preset(s): comma-list, repeatable, and shell-style wildcards "
             "(default: auto-detected by platform)",
    )


def build_overrides(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--toolset", metavar="VERSION", default=None,
        help="Pin the compiler version within the preset's family: a bare version "
             "(clang/gcc -> clang++-N/g++-N on PATH; msvc -> vcvars_ver, e.g. 14.51) or an "
             "explicit compiler path. Not found = hard error. Auto-redirects the build dir so "
             "toolsets don't share a CMake cache.",
    )
    p.add_argument(
        "--build-suffix", metavar="TAG", default=None,
        help="Append '-TAG' to the build folder (build/<preset>-TAG). The go-to for a "
             "toolset matrix: one folder per toolset, side by side.",
    )
    p.add_argument(
        "--build-dir", metavar="PATH", default=None,
        help="Use this build directory instead of build/<preset> (relative to the repo root, "
             "or absolute). For a fully custom layout; single preset only.",
    )


def emsdk(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--emsdk-path", metavar="DIR", default=None,
        help="Path to an emsdk install for the WASM (Emscripten) presets; dev.py applies its "
             "environment itself, so no permanent/--system activation is needed. Falls back to "
             "SC_EMSDK_PATH / EMSDK / emcc-on-PATH.",
    )
