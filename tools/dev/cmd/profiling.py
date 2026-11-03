"""`profiling` — profiling helpers, one subcommand per capability.

`counters` lists the hardware performance counters the current machine can actually measure, via nexus/bench.
`merge` composes the job profiles `--profile` writes, and converts between their formats.
docs/guides/profiling.md is the counters workflow; docs/guides/building-and-testing.md the profiling one.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from tools import dev

from . import args as a
from .context import Context

NAME = "profiling"

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

    merge = psub.add_parser(
        "merge",
        help="Combine job profiles written by --profile into one, re-laning across the whole set",
    )
    merge.add_argument("inputs", nargs="+", metavar="FILE", help="Job profile(s) to combine")
    merge.add_argument("--out", "-o", metavar="FILE", required=True, help="Where to write the result")
    merge.add_argument("--type", choices=("jobs", "chrome-tracing"), default="jobs",
                       dest="out_type", help="Output format (default: jobs)")
    merge.add_argument("--lanes", choices=("global", "per-type"), default="global",
                       dest="out_lanes", help="Lane allocation over the merged set (default: global)")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    if args.profiling_cmd == "counters":
        ctx.warn_known_issues(ctx.resolve_presets(None))
        _run_counters(args, ctx)
    elif args.profiling_cmd == "merge":
        _run_merge(args, ctx)
    else:  # argparse's required=True already rejects anything else
        ctx.die(f"unknown profiling command {args.profiling_cmd!r}")


def _run_merge(args: argparse.Namespace, ctx: Context) -> None:
    """Concatenate job profiles and re-allocate lanes over the union.

    Jobs carry absolute epoch times, so profiles from separate dev.py runs compose without renormalizing.
    A single input is therefore also the converter: pass `--type chrome-tracing` to turn a recorded profile into a loadable trace.
    """
    # Globs are expanded here, not left to the shell: PowerShell passes them through to a native command verbatim.
    paths: list[Path] = []
    for raw in args.inputs:
        if any(c in raw for c in "*?["):
            matched = sorted(Path().glob(raw)) or sorted(ctx.root.glob(raw))
            if not matched:
                ctx.die(f"{raw!r} matched no files")
            paths.extend(matched)
        else:
            paths.append(Path(raw))

    jobs = []
    for path in paths:
        found = dev.profile.load(path)
        if not found:
            ctx.die(f"no jobs in {ctx.rel(path)} (missing, unreadable, or not a dev profile)")
        jobs.extend(found)

    out = Path(args.out)
    dev.profile.emit(jobs, out, fmt=args.out_type, lane_mode=args.out_lanes,
                     argv=["dev.py", "profiling", "merge", *args.inputs])
    print(f"merged {len(paths)} profile(s)", file=sys.stderr)
    dev.report.print_profile_summary(dev.profile.summarize(jobs), str(out))


def _run_counters(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_build_presets(args)
    preset = presets[0]

    # Every nexus binary answers --list-hw-counters, so a consumer without nexus-test uses one of its own.
    # nexus-test where there is one, else a binary already built, so the build below is a no-op.
    candidates = [t for t in ctx.discover(preset, args.emsdk_path)
                  if t.nexus_kinds and t.artifact is not None and t.kind == "EXECUTABLE"]
    if not candidates:
        ctx.die(f"no nexus binary in {preset.name!r} to list the counters with")
    target = min(candidates, key=lambda t: (t.name != "nexus-test", not t.artifact.exists(), t.name))

    results = dev.build(
        [preset], [target.name], root=ctx.root,
        auto_configure=True, mirror=args.mirror_output, verbose=args.verbose,
        emsdk_path=args.emsdk_path,
    )
    if not all(r.ok for r in results):
        ctx.fail_build(results, [preset])

    sys.exit(subprocess.run([str(target.artifact), "--list-hw-counters"]).returncode)
