"""`info` — read-only inspection of resolved compile/link flags and per-file commands.

`build-flags`/`link-flags` print the per-target settings from the CMake File API
(TU-flag-set aware: one block per distinct compile group); `compile-command`
prints the exact per-file invocation from compile_commands.json — the ground
truth the compiler sees.
"""

from __future__ import annotations

import argparse
import shlex
import sys
from pathlib import Path

from tools import dev
from tools.dev import console

from . import args as a
from .context import Context

NAME = "info"


def _add_target(p: argparse.ArgumentParser) -> None:
    a.preset(p)
    a.emsdk(p)
    p.add_argument("target", nargs="+", help="Target(s): comma-list, repeatable, wildcards")


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Inspect resolved compile/link flags and per-file commands")
    info_sub = p.add_subparsers(dest="info_cmd", required=True)

    bf_p = info_sub.add_parser("build-flags", help="Per-target compile flags (one block per distinct flag set)")
    _add_target(bf_p)
    lf_p = info_sub.add_parser("link-flags", help="Per-target linker flags and link libraries")
    _add_target(lf_p)

    cc_p = info_sub.add_parser(
        "compile-command", help="Exact compile command for one source file (from compile_commands.json)"
    )
    a.preset(cc_p)
    a.emsdk(cc_p)
    cc_p.add_argument("file", help="Source file (absolute, repo-relative, or a unique filename)")
    cc_p.add_argument("--raw", action="store_true",
                      help="Print the verbatim single-line command instead of one argument per line")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    match args.info_cmd:
        case "build-flags":
            _flags(args, ctx, compile_=True, link=False)
        case "link-flags":
            _flags(args, ctx, compile_=False, link=True)
        case "compile-command":
            _compile_command(args, ctx)
        case _:  # argparse 'required=True' should prevent this.
            ctx.die(f"unknown info subcommand {args.info_cmd!r}")


def _print_compile_group(ctx: Context, group: dev.CompileGroup, *, sole: bool, index: int) -> None:
    std = f"C++{group.std}" if group.std and group.language == "CXX" else (group.std or "?")
    if sole:
        print(f"  compile flags  ({group.language}, {std}, {len(group.sources)} sources)")
    else:
        print(f"  flag set #{index}  ({group.language}, {std}, {len(group.sources)} sources)")
    if group.defines:
        print(f"    defines:  {', '.join(group.defines)}")
    if group.includes:
        rendered = [(f"[sys] {ctx.rel(Path(p))}" if sys_ else ctx.rel(Path(p))) for p, sys_ in group.includes]
        print(f"    includes: {', '.join(rendered)}")
    for frag in group.flags:
        print(f"    flags:    {frag}")
    if not sole:
        for src in group.sources:
            print(console.dim(f"      - {ctx.rel(Path(src))}"))


def _flags(args: argparse.Namespace, ctx: Context, *, compile_: bool, link: bool) -> None:
    preset = ctx.resolve_presets(args.preset)[0]
    names = ctx.resolve_target_names(preset, args.target, args.emsdk_path) or []
    models = dev.load_target_models(preset.build_dir, preset.build_type)
    for i, name in enumerate(names):
        if i:
            print()
        tf = dev.extract_flags(models[name])
        print(console.bold(f"{tf.name}  [{tf.kind}]") + console.dim(f"  preset={preset.name}"))
        if compile_:
            if not tf.compile_groups:
                print("  (no compile step - not a compiled target)")
            for j, group in enumerate(tf.compile_groups, start=1):
                _print_compile_group(ctx, group, sole=len(tf.compile_groups) == 1, index=j)
        if link:
            if not tf.link_flags and not tf.link_libraries:
                print("  (no link step - static library or non-linked target)")
            else:
                if tf.link_flags:
                    print(f"  link flags:     {' '.join(tf.link_flags)}")
                for libline in tf.link_libraries:
                    print(f"  link library:   {libline}")


def _compile_command(args: argparse.Namespace, ctx: Context) -> None:
    preset = ctx.resolve_presets(args.preset)[0]
    # compile_commands.json is produced by configure; make sure it exists.
    ctx.discover(preset, args.emsdk_path)
    try:
        entries = dev.load_entries(preset.build_dir)
    except FileNotFoundError as e:
        ctx.die(str(e))
    requested = Path(args.file)
    entry = dev.find_entry(entries, requested, ctx.root)
    if entry is None:
        hint = dev.suggest_files(entries, requested)
        msg = f"No compile command for {args.file!r} in {preset.name}."
        if hint:
            msg += " Did you mean:\n  " + "\n  ".join(ctx.rel(Path(h)) for h in hint)
        ctx.die(msg)
    print(console.bold(ctx.rel(Path(entry["file"]))) + console.dim(f"  preset={preset.name}"))
    command = entry.get("command", "")
    if args.raw:
        print(command)
    else:
        for arg in shlex.split(command, posix=False):
            print(f"  {arg}")
