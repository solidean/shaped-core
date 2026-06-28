"""`configure` — run CMake configure for the selected preset(s)."""

from __future__ import annotations

import argparse
import sys

from tools import dev

from . import args as a
from .context import Context

NAME = "configure"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Configure the CMake project")
    a.preset(p)
    a.build_overrides(p)
    a.emsdk(p)
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_build_presets(args)
    results = dev.configure(
        presets, root=ctx.root, force=True, mirror=args.mirror_output, verbose=args.verbose,
        emsdk_path=args.emsdk_path,
    )
    sys.exit(0 if all(r.ok for r in results) else 1)
