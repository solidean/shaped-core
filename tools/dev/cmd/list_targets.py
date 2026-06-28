"""`list-targets` — list discovered targets for the selected preset."""

from __future__ import annotations

import argparse

from . import args as a
from .context import Context

NAME = "list-targets"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="List discovered targets")
    a.preset(p)
    a.emsdk(p)
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_presets(args.preset)
    for t in ctx.discover(presets[0], args.emsdk_path):
        artifact = f"  -> {t.artifact}" if t.artifact else ""
        print(f"{t.name}  [{t.kind}]{artifact}")
