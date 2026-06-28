"""`clean` — remove build directories for one preset, or all of them."""

from __future__ import annotations

import argparse
import sys

from tools import dev
from tools.dev import console

from . import args as a
from .context import Context

NAME = "clean"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Remove build artifacts")
    a.preset(p)
    p.add_argument("--all", action="store_true", help="Remove every preset's build directory")
    p.add_argument("--dry-run", action="store_true", help="Print what would be removed")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    if args.all:
        print(console.dim("clean --all: removing all build/ artifacts"), file=sys.stderr)
        presets = dev.load_presets(ctx.root)
    else:
        presets = ctx.resolve_presets(args.preset)
        print(console.dim(f"clean: preset(s) {', '.join(p.name for p in presets)}"), file=sys.stderr)

    removed_any = False
    for preset in presets:
        removed_any |= dev.remove_build_dir(preset.build_dir, dry_run=args.dry_run)
    if not removed_any:
        print(console.dim("  nothing to remove (already clean)"), file=sys.stderr)
