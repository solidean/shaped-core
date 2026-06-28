"""`format` — run clang-format over libs/ sources."""

from __future__ import annotations

import argparse
import sys

from tools import dev

from .context import Context

NAME = "format"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Format C++ sources with clang-format")
    p.add_argument("--check-only", action="store_true",
                   help="Report non-conforming files and exit non-zero; do not rewrite")
    p.add_argument("--dirty-only", action="store_true",
                   help="Only format git-dirty/untracked libs/ sources (good pre-commit check)")
    p.add_argument("--allow-different-version", action="store_true",
                   help="Downgrade a clang-format version mismatch from error to warning")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    try:
        result = dev.run_format(
            ctx.root,
            check=args.check_only,
            dirty_only=args.dirty_only,
            allow_different_version=args.allow_different_version,
            mirror=args.mirror_output,
            verbose=args.verbose,
        )
    except dev.FormatSetupError as e:
        ctx.die(str(e))
    sys.exit(0 if dev.report.summarize_format(result, ctx.root) else 1)
