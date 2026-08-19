"""`format` — run clang-format over our C++ source roots (see format.source_roots)."""

from __future__ import annotations

import argparse
import sys

from tools import dev

from . import args as a
from .context import Context

NAME = "format"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Format C++ sources with clang-format")
    a.profile(p)
    p.add_argument("--check-only", action="store_true",
                   help="Report non-conforming files and exit non-zero; do not rewrite")
    a.change_scope(p, default_all=True)
    p.add_argument("--allow-different-version", action="store_true",
                   help="Downgrade a clang-format version mismatch from error to warning")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    try:
        result = dev.run_format(
            ctx.root,
            check=args.check_only,
            scope=a.scope_from_args(args),
            allow_different_version=args.allow_different_version,
            mirror=args.mirror_output,
            verbose=args.verbose,
        )
    except (dev.FormatSetupError, dev.ChangeScopeError) as e:
        ctx.die(str(e))
    sys.exit(0 if dev.report.summarize_format(result, ctx.root) else 1)
