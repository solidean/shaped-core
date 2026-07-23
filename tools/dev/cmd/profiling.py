"""`profiling` — profiling helpers for shaped-core.

Today it exposes a single subcommand, `counters`, which lists the hardware performance counters the current
machine can actually measure (via nexus/bench). It is a home for more profiling tooling over time — each
capability is its own subcommand under `dev.py profiling`.
"""

from __future__ import annotations

import argparse
import subprocess
import sys

from tools import dev

from . import args as a
from .context import Context

NAME = "profiling"

# The nexus manual test whose body calls nx::bench::print_hw_counters(). Run by exact name so it fires
# regardless of bucket. Keep in sync with libs/base/nexus/tests/bench-hardware-counters-test.cc.
_COUNTERS_TEST = "nexus bench - list hardware counters"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Profiling helpers (hardware counters, ...)")
    psub = p.add_subparsers(dest="profiling_cmd", required=True)

    counters = psub.add_parser(
        "counters",
        help="List the hardware performance counters measurable on this machine (and any setup still needed)",
    )
    a.preset(counters)
    a.build_overrides(counters)
    a.emsdk(counters)
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    if args.profiling_cmd == "counters":
        _run_counters(args, ctx)
    else:  # argparse's required=True already rejects anything else
        ctx.die(f"unknown profiling command {args.profiling_cmd!r}")


def _run_counters(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_build_presets(args)
    preset = presets[0]

    # Build just the nexus test binary (incremental — a no-op when nothing changed).
    results = dev.build(
        [preset], ["nexus-test"], root=ctx.root,
        auto_configure=True, mirror=args.mirror_output, verbose=args.verbose,
        emsdk_path=args.emsdk_path,
    )
    if not all(r.ok for r in results):
        ctx.fail_build(results, [preset])

    # Locate the built binary and run just the counter-listing test, streaming its output straight through.
    target = next(
        (t for t in ctx.discover(preset, args.emsdk_path) if t.name == "nexus-test" and t.artifact is not None),
        None,
    )
    if target is None:
        ctx.die("could not locate the built nexus-test binary")

    sys.exit(subprocess.run([str(target.artifact), _COUNTERS_TEST]).returncode)
