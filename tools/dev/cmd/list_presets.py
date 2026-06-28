"""`list-presets` — list available build presets."""

from __future__ import annotations

import argparse

from tools import dev

from . import args as a
from .context import Context

NAME = "list-presets"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="List available build presets")
    a.preset(p)
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    for p in dev.load_presets(ctx.root):
        print(f"{p.name}  ({p.build_type or 'no build type'} -> {p.configure_preset})")
