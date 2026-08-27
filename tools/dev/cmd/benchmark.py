"""`benchmark` — list the repo's benchmarks, or build and run the ones you name.

A benchmark is a nexus `BENCHMARK` declaration: a body that measures something with nx::bench::run, in its own
selection bucket.
Every build compiles them and nothing runs them automatically, so this is how they get executed.

Resolution is a cross-binary name lookup, not a target lookup — a benchmark name says nothing about the binary
carrying it, so every `*-test` target is probed.
`--target` narrows which binaries are probed; the match never does.

**This is the one dev.py command that does not default to the repo's usual preset.**
`relwithdebinfo-*` compiles CC_ASSERT in, so a benchmark of cc::vector would measure its bounds checks — a number
that is wrong in a way that looks entirely plausible.
The default here is the platform's `release-*`, and the preset plus the assertion state are printed by the run and
recorded in the JSON.

docs/guides/benchmarking.md is the workflow behind all of it.
"""

from __future__ import annotations

import argparse
import platform
import sys
from pathlib import Path

from tools import dev
from tools.dev import console
from tools.dev.lib.pipeline.benchmarks import collect_benchmarks, select_benchmarks

from . import args as a
from .context import Context

NAME = "benchmark"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run benchmarks, or list them all (defaults to a release preset)")
    a.preset(p)
    a.build_overrides(p)
    a.profile(p)
    p.add_argument("--target", "-t", action="append",
                   help="Test binary target(s) to consider: comma-list, repeatable, wildcards")
    p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    p.add_argument("--json", metavar="FILE",
                   help="Write the full results — every statistic and every sample — to FILE")
    p.add_argument("--rec", metavar="FILE",
                   help="Write a .ccrec of the whole run to FILE, warmup included")
    p.add_argument("--verbose-report", action="store_true",
                   help="Print the full statistics block under every row of a comparison table")
    p.add_argument("--pin", action="store_true",
                   help="Pin the run to one core; reports whether the platform allowed it")
    p.add_argument("--repeat", type=int, default=1, metavar="N",
                   help="Run the selection N times, for run-to-run stability (default: 1)")
    p.add_argument("--timeout", type=float, default=0.0, metavar="SECS",
                   help="Kill a benchmark binary after SECS (default: 0, no limit)")
    p.add_argument("match", nargs="?",
                   help="Benchmark name, or a substring of one. May select several; omit to list them all.")
    return p


def _default_preset_name(ctx: Context) -> str:
    """The platform's release preset — CC_ASSERT off, which is the whole reason this differs from every sibling."""
    name = ctx.policy.default_release.get(platform.system())
    if name is None:
        ctx.die(f"No default release preset for {platform.system()!r}. Use --preset.")
    return name


def run(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_presets(args.preset or [_default_preset_name(ctx)])
    try:
        presets = dev.apply_overrides(
            presets, root=ctx.root,
            toolset=args.toolset, build_suffix=args.build_suffix, build_dir=args.build_dir,
        )
    except dev.ToolsetError as e:
        ctx.die(str(e))
    preset = presets[0]

    wanted = ctx.resolve_target_names(preset, args.target, None) if args.target else None

    # Every test binary is built before probing: a listing can only come from an artifact that exists.
    # Incremental, so this is cheap once the corpus is built, and it is also what keeps a stale name from resolving.
    if not args.no_build:
        targets = ctx.discover(preset, None)
        names = wanted if wanted is not None else [t.name for t in targets if ctx.is_test_target(t)]
        if not names:
            ctx.die("no *-test targets are configured. Is SC_BUILD_TESTS on?")
        results = dev.build([preset], names, root=ctx.root,
                            auto_configure=not args.no_configure,
                            mirror=args.mirror_output, verbose=args.verbose)
        if not all(r.ok for r in results):
            ctx.fail_build(results, [preset])

    # Discovered after the build: a first-time configure knows the target but has not linked its artifact yet.
    targets = ctx.discover(preset, None)
    benchmarks = collect_benchmarks(preset, targets, root=ctx.root, binary_names=wanted)

    if args.match is None:
        _print_listing(ctx, benchmarks, preset)
        return

    selected, diagnostic = select_benchmarks(benchmarks, args.match)
    if not selected:
        ctx.die(diagnostic)

    # Grouped by binary so one launch runs every benchmark it carries, which is what lets several rows share one
    # process — and one system summary, and one .ccrec.
    by_target: dict[str, list] = {}
    for b in selected:
        by_target.setdefault(b.target, []).append(b)

    print(console.dim(f"{len(selected)} benchmark(s) in {len(by_target)} binary(s), preset {preset.name}"),
          file=sys.stderr)

    failed = False
    for iteration in range(max(1, args.repeat)):
        if args.repeat > 1:
            print(console.dim(f"--- repeat {iteration + 1}/{args.repeat}"), file=sys.stderr)

        for target in sorted(by_target):
            artifact = next((t.artifact for t in targets if t.name == target and t.artifact), None)
            if artifact is None:
                ctx.die(f"target {target!r} has no built artifact for preset {preset.name!r}")

            command = [str(artifact), "--benchmarks"]
            for b in by_target[target]:
                command.append(b.name)

            # Suffixed per binary and per repeat, so several binaries or several runs never overwrite one another.
            if args.json:
                command += ["--benchmark-json", _sidecar_path(args.json, target, iteration, args.repeat)]
            if args.rec:
                command += ["--benchmark-rec", _sidecar_path(args.rec, target, iteration, args.repeat)]
            if args.verbose_report:
                command.append("--benchmark-verbose")
            if args.pin:
                command.append("--benchmark-pin")

            # Mirrored: reading the report as it comes out is the entire point of the command.
            result = dev.run_step(
                command, step_type="benchmark", name=target,
                build_dir=preset.build_dir, cwd=ctx.root,
                timeout=args.timeout if args.timeout else None,
                mirror=True, verbose=args.verbose,
            )
            if result.returncode != 0:
                failed = True

    sys.exit(1 if failed else 0)


def _sidecar_path(base: str, target: str, iteration: int, repeat: int) -> str:
    """`base` with the binary — and the repeat, where there is more than one — folded into its stem."""
    path = Path(base)
    stem = path.stem
    if repeat > 1:
        stem = f"{stem}.{iteration + 1}"
    return str(path.with_name(f"{stem}.{target}{path.suffix}"))


def _print_listing(ctx: Context, benchmarks: list, preset) -> None:
    """Print every benchmark, grouped by the binary carrying it."""
    if not benchmarks:
        ctx.die("no benchmarks found. Has anything declared a BENCHMARK?")

    by_target: dict[str, list] = {}
    for b in benchmarks:
        by_target.setdefault(b.target, []).append(b)

    for target in sorted(by_target):
        print(console.dim(target))
        for b in by_target[target]:
            print(f"  {b.name}")
            print(console.dim(f"      {ctx.rel(Path(b.file))}:{b.line}"))

    print(console.dim(f"\n{len(benchmarks)} benchmark(s), preset {preset.name}."
                      f" Run some with: dev.py benchmark <match>"))
