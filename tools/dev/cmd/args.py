"""Shared argparse fragments reused across command subparsers.

A command owns its own subparser; these keep the flags several of them share — preset selection, the build-dir overrides, the emsdk path, the profiling flags — defined once.
`change_scope` and `scope_from_args` are a pair: the flags, and the ChangeScope they mean.
"""

from __future__ import annotations

import argparse

from tools import dev


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


def profile(p: argparse.ArgumentParser) -> None:
    """The profiling flags, repeated per subcommand so they bind on either side of it.

    `SUPPRESS` is load-bearing: a normal default would have the subparser write its own None over a value given before the subcommand.
    dev.py declares the same three flags globally, and these only shadow them when actually passed.
    """
    p.add_argument(
        "--profile", metavar="FILE", default=argparse.SUPPRESS,
        help="Write a job profile of this run to FILE: one record per subprocess, compile "
             "edge, lint job and in-process phase, with lanes allocated for overlap.",
    )
    p.add_argument(
        "--profile-type", choices=("jobs", "chrome-tracing"), default=argparse.SUPPRESS,
        help="Profile format: 'jobs' (default) is the raw records, 'chrome-tracing' converts "
             "them to a trace https://ui.perfetto.dev loads directly.",
    )
    p.add_argument(
        "--profile-lanes", choices=("global", "per-type"), default=argparse.SUPPRESS,
        help="Lane allocation: 'global' (default) packs every job into one pool, 'per-type' "
             "gives each job type its own pool and its own track in the trace.",
    )


def emsdk(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--emsdk-path", metavar="DIR", default=None,
        help="Path to an emsdk install for the WASM (Emscripten) presets; dev.py applies its "
             "environment itself, so no permanent/--system activation is needed. Falls back to "
             "SC_EMSDK_PATH / EMSDK / emcc-on-PATH.",
    )


def change_scope(p: argparse.ArgumentParser, *, default_all: bool) -> None:
    """The `--dirty-only` / `--commit` pair that narrows a lint or format run to one change set.

    `default_all` says what no flag at all means: True for a command that sweeps the whole tree by default (`format`, `lint`),
    False for one that is already dirty-only by default (`check`, which spells the whole tree `--all` instead).
    Both flags replace the default scope, so they go in a mutually exclusive group.
    """
    group = p.add_mutually_exclusive_group()
    if default_all:
        group.add_argument("--dirty-only", action="store_true",
                           help="Only the next commit's files — git-dirty and untracked")
    else:
        group.add_argument("--all", action="store_true",
                           help="Widen from dirty-only to the whole tree")
    group.add_argument("--commit", metavar="REV", default=None,
                       help="Use a commit or `A..B` range instead of the working tree; "
                            "a single commit means its first-parent diff, so a merge yields everything it brought in")


def scope_from_args(args: argparse.Namespace) -> dev.ChangeScope | None:
    """The ChangeScope a `change_scope` flag pair asked for, where None means the whole tree.

    Does not validate the revision — the caller decides where a bad one should surface.
    """
    if args.commit is not None:
        return dev.ChangeScope(args.commit)
    if hasattr(args, "dirty_only"):
        return dev.ChangeScope() if args.dirty_only else None
    return None if args.all else dev.ChangeScope()
