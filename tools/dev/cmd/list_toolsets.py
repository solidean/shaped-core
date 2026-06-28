"""`list-toolsets` — list installed compiler toolsets per family (for --toolset)."""

from __future__ import annotations

import argparse
import platform

from tools import dev
from tools.dev import console

from .context import Context

NAME = "list-toolsets"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    return sub.add_parser(NAME, help="List installed compiler toolsets per family (for --toolset)")


def run(args: argparse.Namespace, ctx: Context) -> None:
    data = dev.list_toolsets()

    print(console.bold("msvc"))
    if not data["msvc"]:
        why = "" if platform.system() == "Windows" else "  (Windows only)"
        print(console.dim(f"  none found{why}"))
    for inst in data["msvc"]:
        tag = console.yellow(" [prerelease]") if inst["prerelease"] else ""
        print(f"  {inst['name']}{tag}  {console.dim(inst['path'])}")
        if not inst["toolsets"]:
            print(console.dim("    (no C++ toolset installed)"))
        for version in inst["toolsets"]:
            print(f"    {version:<16} --toolset {dev.toolset_hint(version)}")

    for family in ("clang", "gcc"):
        print(console.bold(family))
        entries = data[family]
        if not entries:
            print(console.dim("  none found on PATH"))
        for e in entries:
            hint = f"--toolset {e['toolset']}" if e["toolset"] else console.dim("(use an explicit path)")
            banner = f"  {console.dim(e['version'])}" if e["version"] else ""
            print(f"  {e['name']:<16} {hint}{banner}")
