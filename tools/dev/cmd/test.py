"""`test` — build (incrementally) and run test binaries for the selected preset(s)."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from tools import dev

from tools.dev.lib.toolchain import jsruntime as jsr

from . import args as a
from .context import Context

NAME = "test"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run tests")
    a.preset(p)
    a.build_overrides(p)
    a.emsdk(p)
    a.jsruntime(p)
    a.profile(p)
    p.add_argument("--target", "-t", action="append",
                   help="Test binary target(s): comma-list, repeatable, wildcards")
    p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    p.add_argument("--timeout", type=float, default=60.0, metavar="SECS",
                   help="Per-binary timeout in seconds (default: 60; 0 disables). The binary is "
                        "killed and reported as failed if it exceeds it.")
    p.add_argument("--jobs", "-j", type=int, metavar="N",
                   help="Upper bound on how many tests run at once, forwarded to the runner (nexus understands it); "
                        "0 means hardware concurrency. dev.py has to own the flag rather than let it fall through to "
                        "runner_args: an unrecognized '--jobs N' leaves N as a bare token, which the test_name "
                        "positional then swallows, and the run silently filters instead of narrowing its width. "
                        "A low count is what reproduces a small CI runner's scheduling on a wide dev machine.")
    p.add_argument("--test-args", metavar="LINE",
                   help="A command line for the selected test itself, reachable from its body through "
                        "nx::test_args(). Forwarded to the runner as one string and tokenized there, "
                        "which is why it survives dev.py's own '--' handling. It replaces whatever the test "
                        "declared with nx::config::args, and applies to every test the run selects.")
    p.add_argument("--repeat", type=int, default=1, metavar="N",
                   help="Run the selection up to N times, stopping at the first failing iteration "
                        "(default: 1). For chasing a flake: the build and discovery happen once, and "
                        "stopping on failure is what leaves that run's logs and XML on disk to read.")
    xml_group = p.add_mutually_exclusive_group()
    xml_group.add_argument("--merged-xml-report", metavar="FILE",
                           help="Also merge the per-binary JUnit XML into a single report at FILE")
    xml_group.add_argument("--no-xml-reports", action="store_true",
                           help="Do not write any JUnit XML result files (per-binary XML is on by default)")
    p.add_argument("test_name", nargs="?",
                   help="Test name, binary, or source file to run (auto-discovers the binary). "
                        "A pattern matching no test name is retried as a glob over the tests' source files, "
                        "so 'vector-test.cc' or 'libs/base/clean-core/tests/memory/*' select by file.")
    # Args dev.py does not recognize are forwarded verbatim to the test binary, collected by the top-level parse_known_args into args.runner_args.
    # `-c <section>` scopes into a section or instance, repeated to descend a path.
    # An optional leading `--` is dropped, and dev.py's own options still bind wherever they sit relative to the test name.
    return p


def run(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_build_presets(args)
    primary = presets[0]

    # parse_known_args already drops the first `--`.
    # Strip a stray leading one as well, so an explicit `test <name> -- -c foo` never leaks the separator through.
    runner_args = list(args.runner_args or [])
    if runner_args and runner_args[0] == "--":
        runner_args = runner_args[1:]

    # Prepended, so an explicit `-- --jobs 4` after it still wins by being parsed later.
    if args.jobs is not None:
        runner_args = ["--jobs", str(args.jobs), *runner_args]

    # One string, deliberately: the runner tokenizes it, so the test's own flags never have to survive
    # dev.py's argument handling — which strips a leading `--` and would otherwise eat the separator.
    if args.test_args is not None:
        runner_args = ["--test-args", args.test_args, *runner_args]

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
            runtime=jsr.JsRuntimeRequest.from_args(args),
        )
        if not all(r.ok for r in results):
            ctx.fail_build(results, presets)

    # Determine which test binaries to run and the optional test-name filter.
    all_targets = ctx.discover(primary, args.emsdk_path)
    wanted = ctx.resolve_target_names(primary, args.target, args.emsdk_path) if args.target else None
    # Example binaries are candidates too: one may carry ordinary TESTs for machinery it grew, and those are tests like any other.
    # Its EXAMPLEs are in their own bucket and never run here.
    binary_names, test_name, err = dev.select_test_binaries(
        all_targets, is_test=lambda t: ctx.is_test_target(t) or ctx.is_example_target(t),
        wanted_names=wanted, name_arg=args.test_name, target_label=args.target,
    )
    if err:
        ctx.die(err)

    # ...and then the example binaries that turn out to have none are dropped, so a sweep does not fail on them.
    binary_names = dev.drop_testless_examples(
        primary, all_targets, binary_names,
        is_example=ctx.is_example_target, test_name=test_name, root=ctx.root, extra_args=runner_args,
    )

    # With a filter, only the binaries that actually contain a matching test run, queried via nexus' --list-tests-json on the primary preset.
    # A binary that cannot answer the query is kept and run as before.
    if test_name:
        binary_names, diag = dev.select_eligible_binaries(
            primary, all_targets, binary_names,
            test_name=test_name, root=ctx.root, extra_args=runner_args,
        )
        if diag:
            ctx.die(diag)

    # Repeat runs the SAME selection again — build and discovery above happen once, so an iteration costs only the binaries.
    # It stops at the first failure rather than tallying a rate: the run that failed is then the one whose logs and XML are still on disk, which is the whole point.
    repeat = max(1, args.repeat)
    for iteration in range(1, repeat + 1):
        if repeat > 1:
            print(dev.console.dim(f"--- repeat {iteration}/{repeat}"), file=sys.stderr)

        records = dev.test(
            presets,
            binary_names,
            root=ctx.root,
            test_name=test_name,
            extra_args=runner_args,
            timeout=args.timeout if args.timeout else None,
            write_xml=not args.no_xml_reports,
            mirror=args.mirror_output,
            verbose=args.verbose,
            emsdk_path=args.emsdk_path,
        )

        if any(r["returncode"] != 0 for r in records):
            if repeat > 1:
                print(dev.console.red(f"failed on iteration {iteration} of {repeat}"), file=sys.stderr)
            break

    if args.merged_xml_report:
        parts = [Path(r["artifact"]).parent / f"{Path(r['artifact']).name}.results.xml" for r in records]
        dev.merge_junit(parts, Path(args.merged_xml_report))

    sys.exit(0 if dev.report.summarize_tests(records, presets, ctx.root) else 1)
