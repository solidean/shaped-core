"""`test` — build (incrementally) and run test binaries for the selected preset(s)."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from tools import dev

from . import args as a
from .context import Context

NAME = "test"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run tests")
    a.preset(p)
    a.build_overrides(p)
    a.emsdk(p)
    p.add_argument("--target", "-t", action="append",
                   help="Test binary target(s): comma-list, repeatable, wildcards")
    p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    p.add_argument("--timeout", type=float, default=60.0, metavar="SECS",
                   help="Per-binary timeout in seconds (default: 60; 0 disables). The binary is "
                        "killed and reported as failed if it exceeds it.")
    xml_group = p.add_mutually_exclusive_group()
    xml_group.add_argument("--merged-xml-report", metavar="FILE",
                           help="Also merge the per-binary JUnit XML into a single report at FILE")
    xml_group.add_argument("--no-xml-reports", action="store_true",
                           help="Do not write any JUnit XML result files (per-binary XML is on by default)")
    p.add_argument("test_name", nargs="?",
                   help="Specific test name or binary to run (auto-discovers the binary)")
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_build_presets(args)
    primary = presets[0]

    # Optionally build first (incremental — fast when nothing changed).
    if not args.no_build:
        target_names = (
            ctx.resolve_target_names(primary, args.target, args.emsdk_path)
            if not args.no_configure else None
        )
        results = dev.build(
            presets, target_names, root=ctx.root,
            auto_configure=not args.no_configure,
            mirror=args.mirror_output, verbose=args.verbose,
            emsdk_path=args.emsdk_path,
        )
        if not all(r.ok for r in results):
            ctx.fail_build(results, presets)

    # Determine which test binaries to run and the optional test-name filter.
    all_targets = ctx.discover(primary, args.emsdk_path)
    wanted = ctx.resolve_target_names(primary, args.target, args.emsdk_path) if args.target else None
    binary_names, test_name, err = dev.select_test_binaries(
        all_targets, is_test=ctx.is_test_target,
        wanted_names=wanted, name_arg=args.test_name, target_label=args.target,
    )
    if err:
        ctx.die(err)

    # With a name/pattern filter, only run binaries that actually contain a matching test (queried via nexus'
    # --list-tests-json on the primary preset). An unfiltered sweep is unchanged — every binary runs and an
    # empty one fails loudly. A filter that matches nothing anywhere fails with a diagnostic instead of a
    # spurious "All 0 passed". Binaries that can't answer the query are kept and run as before.
    if test_name:
        binary_names, diag = dev.select_eligible_binaries(
            primary, all_targets, binary_names,
            test_name=test_name, root=ctx.root,
        )
        if diag:
            ctx.die(diag)

    records = dev.test(
        presets,
        binary_names,
        root=ctx.root,
        test_name=test_name,
        timeout=args.timeout if args.timeout else None,
        write_xml=not args.no_xml_reports,
        mirror=args.mirror_output,
        verbose=args.verbose,
        emsdk_path=args.emsdk_path,
    )

    if args.merged_xml_report:
        parts = [Path(r["artifact"]).parent / f"{Path(r['artifact']).name}.results.xml" for r in records]
        dev.merge_junit(parts, Path(args.merged_xml_report))

    sys.exit(0 if dev.report.summarize_tests(records, presets, ctx.root) else 1)
