"""`diagnose` — diagnose tooling (currently clangd) for a source file."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from tools import dev
from tools.dev import console

from . import args as a
from .context import Context

NAME = "diagnose"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Diagnose tooling (e.g. clangd) for a file")
    diag_sub = p.add_subparsers(dest="diagnose_target", required=True)
    clangd_p = diag_sub.add_parser(
        "clangd", help="Show clangd diagnostics for a file (uses build/compile_commands.json)"
    )
    a.preset(clangd_p)
    clangd_p.add_argument(
        "file", help="Source file to check (its compile flags come from the compilation database)"
    )
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    if args.diagnose_target == "clangd":
        _clangd(args, ctx)
    else:  # argparse 'required=True' should prevent this.
        ctx.die(f"unknown diagnose target {args.diagnose_target!r}")


def _clangd(args: argparse.Namespace, ctx: Context) -> None:
    file = Path(args.file)
    if not file.is_absolute():
        file = Path.cwd() / file
    file = file.resolve()
    if not file.is_file():
        ctx.die(f"no such file: {args.file}")

    clangd_bin = dev.clangd.find_clangd()
    if clangd_bin is None:
        ctx.die("clangd not found on PATH. Install LLVM/clangd or add it to PATH.")

    # Default: reproduce the editor exactly — let clangd discover the database the
    # same way it does in the IDE (via .clangd's CompilationDatabase and its own
    # upward search). That way a misconfigured .clangd shows up here too, instead
    # of being masked. With --preset, force that preset's per-preset database.
    if args.preset:
        preset = ctx.resolve_presets(args.preset)[0]
        cc_dir = preset.build_dir
        label = f"{preset.name} ({ctx.rel(cc_dir)})"
        if not (cc_dir / "compile_commands.json").is_file():
            ctx.die(f"no compile_commands.json at {ctx.rel(cc_dir)} - run: uv run dev.py configure --preset {preset.name}")
    else:
        cc_dir = None
        label = "clangd auto-discovery (.clangd)"

    print(console.dim(f"clangd: checking {ctx.rel(file)} against {label}"), file=sys.stderr)
    try:
        result = dev.clangd.check_file(clangd_bin, file, compile_commands_dir=cc_dir, timeout=120)
    except subprocess.TimeoutExpired:
        ctx.die("clangd --check timed out")

    if not result.found_database:
        print(
            console.yellow(
                f"WARNING: clangd found no compilation database for {ctx.rel(file)} and used generic "
                f"fallback flags (no project includes), so diagnostics below are unreliable. This is "
                f"the same failure the editor hits. Check .clangd's CompilationDatabase points at the "
                f"build/ directory, and run: uv run dev.py configure"
            ),
            file=sys.stderr,
        )

    if args.verbose:
        print(result.log, file=sys.stderr)

    rel = ctx.rel(file)
    for d in result.diagnostics:
        line = f"{rel}:{d.line}: {d.severity}: {d.message} [{d.code}]"
        if d.severity == "error":
            line = console.red(line)
        elif d.severity == "warning":
            line = console.yellow(line)
        print(line)
    sys.stdout.flush()  # keep diagnostics (stdout) ahead of the summary (stderr)

    errors = len(result.errors)
    warnings = len(result.warnings)
    if not result.diagnostics:
        print(console.green("\nNo diagnostics."), file=sys.stderr)
    else:
        summary = f"\n{errors} error(s), {warnings} warning(s)"
        print(console.red(summary) if errors else console.yellow(summary), file=sys.stderr)
    sys.exit(1 if errors else 0)
