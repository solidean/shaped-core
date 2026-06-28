"""`doctor` — sanity-check the toolchain for the selected preset."""

from __future__ import annotations

import argparse
import dataclasses
import sys

from tools import dev
from tools.dev import console

from . import args as a
from .context import Context

NAME = "doctor"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Sanity-check the toolchain")
    a.preset(p)
    p.add_argument(
        "--toolset", metavar="VERSION", default=None,
        help="Report the compiler this toolset resolves to (same value as build/test --toolset), "
             "instead of the preset default.",
    )
    a.emsdk(p)
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    # Resolve the preset (falling back to the platform default) and attach --toolset WITHOUT
    # validating it — doctor should *report* a missing/wrong toolset, not hard-fail on it.
    preset = dataclasses.replace(ctx.resolve_presets(args.preset)[0], toolset=args.toolset)
    checks = dev.doctor(ctx.root, preset=preset, emsdk_path=args.emsdk_path)
    all_ok = True
    for label, ok, detail in checks:
        # ok is True (pass), False (fail), or None for an advisory (neither).
        if ok is None:
            mark = console.yellow("SKIP")
        elif ok:
            mark = console.green("OK  ")
        else:
            mark = console.red("FAIL")
            all_ok = False
        print(f"  [{mark}] {label}: {detail}")
    sys.exit(0 if all_ok else 1)
