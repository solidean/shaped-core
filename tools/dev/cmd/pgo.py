"""`pgo` — profile-guided optimization (instrument / train / optimize / measure)."""

from __future__ import annotations

import argparse
import platform
import sys

from tools import dev
from tools.dev import console

from . import args as a
from .context import Context

NAME = "pgo"


def _add_timeout(p: argparse.ArgumentParser) -> None:
    p.add_argument("--timeout", type=float, default=0.0, metavar="SECS",
                   help="Per-binary timeout in seconds (default: 0 = disabled; benchmarks run long)")


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Profile-guided optimization (instrument/train/optimize/measure)")
    pgo_sub = p.add_subparsers(dest="pgo_cmd", required=True)

    run_p = pgo_sub.add_parser("run", help="Full pipeline: instrument -> train -> optimize -> measure")
    a.preset(run_p)
    _add_timeout(run_p)
    run_p.add_argument("--no-measure", action="store_true",
                       help="Stop after the optimized build; skip the baseline-vs-PGO measurement")

    inst_p = pgo_sub.add_parser("instrument", help="Build the instrumented (-fprofile-generate) preset")
    a.preset(inst_p)

    train_p = pgo_sub.add_parser("train", help="Run guide benchmarks on the instrumented build, merge profile")
    a.preset(train_p)
    _add_timeout(train_p)

    opt_p = pgo_sub.add_parser("optimize", help="Build the optimized (-fprofile-use) preset from the profile")
    a.preset(opt_p)

    meas_p = pgo_sub.add_parser("measure", help="Run guide benchmarks on baseline + PGO and diff metrics")
    a.preset(meas_p)
    _add_timeout(meas_p)
    return p


def _platform_preset(ctx: Context, table: dict[str, str], what: str) -> dev.Preset:
    name = table.get(platform.system())
    if name is None:
        ctx.die(f"No default {what} preset for {platform.system()!r}. Use --preset.")
    return ctx.resolve_presets([name])[0]


def _presets(args: argparse.Namespace, ctx: Context) -> tuple[dev.Preset, dev.Preset, dev.Preset]:
    """Resolve (generate, use, baseline) presets. --preset overrides the generate/use pair."""
    if args.preset:
        gen = ctx.resolve_presets(args.preset)[0]
        # Derive the matching use preset from the generate name (…-generate… → …-use…).
        use = ctx.resolve_presets([gen.name.replace("generate", "use")])[0]
    else:
        gen = _platform_preset(ctx, ctx.policy.pgo_generate, "pgo-generate")
        use = _platform_preset(ctx, ctx.policy.pgo_use, "pgo-use")
    baseline = _platform_preset(ctx, ctx.policy.pgo_baseline, "pgo baseline")
    return gen, use, baseline


def _binary_names(ctx: Context, preset: dev.Preset) -> list[str]:
    """Test binaries to drive (every *-test); guide-benchmark-less ones simply no-op."""
    return [t.name for t in ctx.discover(preset) if ctx.is_test_target(t)]


def run(args: argparse.Namespace, ctx: Context) -> None:
    match args.pgo_cmd:
        case "run":
            _run(args, ctx)
        case "instrument":
            _instrument(args, ctx)
        case "train":
            _train(args, ctx)
        case "optimize":
            _optimize(args, ctx)
        case "measure":
            _measure(args, ctx)
        case _:  # argparse 'required=True' should prevent this.
            ctx.die(f"unknown pgo subcommand {args.pgo_cmd!r}")


def _instrument(args: argparse.Namespace, ctx: Context) -> None:
    gen, _use, _base = _presets(args, ctx)
    results = dev.pgo_instrument(gen, root=ctx.root, mirror=args.mirror_output, verbose=args.verbose)
    if not all(r.ok for r in results):
        ctx.fail_build(results, [gen])
    print(console.green(f"Instrumented build ready: {gen.name}"), file=sys.stderr)
    sys.exit(0)


def _train(args: argparse.Namespace, ctx: Context) -> None:
    gen, _use, _base = _presets(args, ctx)
    binaries = _binary_names(ctx, gen)
    try:
        result = dev.pgo_train(
            gen, binaries, root=ctx.root, timeout=args.timeout if args.timeout else None,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except dev.PgoError as e:
        ctx.die(str(e))
    sys.exit(0 if dev.report.summarize_pgo({"ok": result["ok"], "train": result, "measure": None}, ctx.root) else 1)


def _optimize(args: argparse.Namespace, ctx: Context) -> None:
    _gen, use, _base = _presets(args, ctx)
    try:
        results = dev.pgo_optimize(use, root=ctx.root, mirror=args.mirror_output, verbose=args.verbose)
    except dev.PgoError as e:
        ctx.die(str(e))
    if not all(r.ok for r in results):
        ctx.fail_build(results, [use])
    print(console.green(f"Optimized (PGO) build ready: {use.name}"), file=sys.stderr)
    sys.exit(0)


def _measure(args: argparse.Namespace, ctx: Context) -> None:
    _gen, use, baseline = _presets(args, ctx)
    binaries = _binary_names(ctx, use)
    try:
        result = dev.pgo_measure(
            baseline, use, binaries, root=ctx.root, timeout=args.timeout if args.timeout else None,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except dev.PgoError as e:
        ctx.die(str(e))
    sys.exit(0 if dev.report.summarize_pgo({"ok": True, "train": None, "measure": result}, ctx.root) else 1)


def _run(args: argparse.Namespace, ctx: Context) -> None:
    gen, use, baseline = _presets(args, ctx)
    binaries = _binary_names(ctx, gen)
    try:
        result = dev.pgo_run(
            gen, use, baseline, binaries, root=ctx.root, measure=not args.no_measure,
            timeout=args.timeout if args.timeout else None,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except dev.PgoError as e:
        ctx.die(str(e))
    sys.exit(0 if dev.report.summarize_pgo(result, ctx.root) else 1)
