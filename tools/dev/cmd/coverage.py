"""`coverage` — LLVM source-based test coverage (run / merge / report).

Three phases map to subcommands: `run` (build + run tests + merge + report),
`merge` (combine several presets' merged data), and `report` (re-post-process
existing data without re-running). The raw `llvm-cov export` JSON lands as a
`.llvm-cov.json` sidecar in the build dir for future tooling.
"""

from __future__ import annotations

import argparse
import platform
import sys
from pathlib import Path

from tools import dev

from . import args as a
from .context import Context

NAME = "coverage"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Collect LLVM source-based test coverage")
    cov_sub = p.add_subparsers(dest="coverage_cmd", required=True)

    run_p = cov_sub.add_parser("run", help="Build + run instrumented tests, then merge & report")
    a.preset(run_p)
    run_p.add_argument("--target", "-t", action="append",
                       help="Test binary target(s): comma-list, repeatable, wildcards")
    run_p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    run_p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    run_p.add_argument("--html", action="store_true", help="Also write an llvm-cov HTML report")
    run_p.add_argument("--timeout", type=float, default=60.0, metavar="SECS",
                       help="Per-binary timeout in seconds (default: 60; 0 disables)")
    run_p.add_argument("pattern", nargs="?",
                       help="Specific test name or binary to run (auto-discovers the binary)")

    merge_p = cov_sub.add_parser("merge", help="Combine several presets' coverage into one report")
    a.preset(merge_p)
    merge_p.add_argument("--output", "-o", metavar="DIR",
                         help="Output directory (default: build/coverage-merged)")
    merge_p.add_argument("--html", action="store_true", help="Also write an llvm-cov HTML report")

    report_p = cov_sub.add_parser("report", help="Re-post-process existing coverage (no test run)")
    a.preset(report_p)
    report_p.add_argument("--html", action="store_true", help="Also write an llvm-cov HTML report")
    return p


def _default_preset_name(ctx: Context) -> str:
    name = ctx.policy.coverage_build.get(platform.system())
    if name is None:
        ctx.die(f"No default coverage preset for {platform.system()!r}. Use --preset.")
    return name


def _resolve_presets(ctx: Context, specs: list[str] | None) -> list[dev.Preset]:
    """Resolve --preset for coverage, defaulting to the platform coverage preset."""
    return ctx.resolve_presets(specs or [_default_preset_name(ctx)])


def run(args: argparse.Namespace, ctx: Context) -> None:
    match args.coverage_cmd:
        case "run":
            _run(args, ctx)
        case "merge":
            _merge(args, ctx)
        case "report":
            _report(args, ctx)
        case _:  # argparse 'required=True' should prevent this.
            ctx.die(f"unknown coverage subcommand {args.coverage_cmd!r}")


def _run(args: argparse.Namespace, ctx: Context) -> None:
    presets = _resolve_presets(ctx, args.preset)
    primary = presets[0]

    if not args.no_build:
        results = dev.build(
            presets, None, root=ctx.root, auto_configure=not args.no_configure,
            mirror=args.mirror_output, verbose=args.verbose,
        )
        if not all(r.ok for r in results):
            ctx.fail_build(results, presets)

    all_targets = ctx.discover(primary)
    wanted = ctx.resolve_target_names(primary, args.target) if args.target else None
    binary_names, test_name, err = dev.select_test_binaries(
        all_targets, is_test=ctx.is_test_target,
        wanted_names=wanted, name_arg=args.pattern, target_label=args.target,
    )
    if err:
        ctx.die(err)
    try:
        cov_results = dev.coverage_run(
            presets, binary_names, root=ctx.root, test_name=test_name, html=args.html,
            timeout=args.timeout if args.timeout else None,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except dev.CoverageToolError as e:
        ctx.die(str(e))
    sys.exit(0 if dev.report.summarize_coverage(cov_results, ctx.root) else 1)


def _report(args: argparse.Namespace, ctx: Context) -> None:
    presets = _resolve_presets(ctx, args.preset)
    binary_names = [t.name for t in ctx.discover(presets[0]) if ctx.is_test_target(t)]
    try:
        results = dev.coverage_report(
            presets, binary_names, root=ctx.root, html=args.html,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except (dev.CoverageToolError, FileNotFoundError) as e:
        ctx.die(str(e))
    sys.exit(0 if dev.report.summarize_coverage(results, ctx.root) else 1)


def _merge(args: argparse.Namespace, ctx: Context) -> None:
    if not args.preset:
        ctx.die("coverage merge needs at least one --preset (which presets' data to combine)")
    presets = ctx.resolve_presets(args.preset)
    names_by_preset = {p.name: [t.name for t in ctx.discover(p) if ctx.is_test_target(t)] for p in presets}
    output_dir = Path(args.output) if args.output else (ctx.root / "build" / "coverage-merged")
    try:
        result = dev.coverage_merge(
            presets, names_by_preset, root=ctx.root, output_dir=output_dir, html=args.html,
            mirror=args.mirror_output, verbose=args.verbose,
        )
    except (dev.CoverageToolError, FileNotFoundError) as e:
        ctx.die(str(e))
    sys.exit(0 if dev.report.summarize_coverage([result], ctx.root) else 1)
