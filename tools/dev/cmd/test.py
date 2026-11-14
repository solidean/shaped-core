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

DEFAULT_TIMEOUT_S = 60.0

# A thorough run is the full-strength version of every test, sized to what is worth waiting for, so the guard grows with it.
THOROUGH_TIMEOUT_FACTOR = 10


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run tests")
    a.preset(p)
    a.build_overrides(p)
    a.server(p)
    a.emsdk(p)
    a.jsruntime(p)
    a.profile(p)
    p.add_argument("--target", "-t", action="append",
                   help="Test binary target(s): comma-list, repeatable, wildcards")
    p.add_argument("--no-build", action="store_true", help="Skip the automatic build step")
    p.add_argument("--no-configure", action="store_true", help="Skip automatic configure step")
    p.add_argument("--timeout", type=float, default=None, metavar="SECS",
                   help=f"Per-binary timeout in seconds (default: {DEFAULT_TIMEOUT_S:g}, or "
                        f"{DEFAULT_TIMEOUT_S * THOROUGH_TIMEOUT_FACTOR:g} under --thorough; 0 disables). "
                        "A binary that declares a longer one (sc_nexus_binary TIMEOUT) gets that instead, unless "
                        "this is given, which holds for every binary. "
                        "The binary is killed and reported as failed if it exceeds it.")
    p.add_argument("--jobs", "-j", type=int, metavar="N",
                   help="Upper bound on how many tests run at once, forwarded to the runner (nexus understands it); "
                        "0 means hardware concurrency. dev.py has to own the flag rather than let it fall through to "
                        "runner_args: an unrecognized '--jobs N' leaves N as a bare token, which the test_name "
                        "positional then swallows, and the run silently filters instead of narrowing its width. "
                        "A low count is what reproduces a small CI runner's scheduling on a wide dev machine.")
    p.add_argument("--seed", type=int, metavar="N",
                   help="The run seed, forwarded to the runner: test order, invocation order and every "
                        "nx::test_seed derive from it. Every nexus run prints the one it drew, so a failure "
                        "reproduces with the seed from its log.")
    p.add_argument("--watchdog", type=float, default=None, metavar="SECS",
                   help="After SECS with no test starting or finishing, the runner reports the hung run under a "
                        "[nexus watchdog] marker — outstanding work, running and awaiting tests, every thread's "
                        "stack and open scopes, then the recording — and exits with code 4. "
                        "Forwarded to the runner. Defaults to half the per-binary timeout, so the report lands "
                        "before the timeout kills the binary; 0 turns it off.")
    p.add_argument("--test-args", metavar="LINE",
                   help="A command line for the selected test itself, reachable from its body through "
                        "nx::test_args(). Forwarded to the runner as one string and tokenized there, "
                        "which is why it survives dev.py's own '--' handling. It replaces whatever the test "
                        "declared with nx::config::args, and applies to every test the run selects.")
    p.add_argument("--thorough", action="store_true",
                   help="Run every test at full strength rather than narrowed to what a default run affords: "
                        "tests read it through nx::is_thorough() and raise their seeds, caps and input sizes. "
                        "Forwarded to the runner as --thorough. "
                        f"Raises the per-binary timeout to {DEFAULT_TIMEOUT_S * THOROUGH_TIMEOUT_FACTOR:g} s "
                        "unless --timeout is given.")
    p.add_argument("--smoke", action="store_true",
                   help="Let a heavy test narrow to its smallest case, as `check` asks of every preset but its default "
                        "one: tests read it through nx::is_smoke(). Forwarded to the runner as --smoke; "
                        "--thorough wins over it.")
    p.add_argument("--shard", metavar="K/N",
                   help="Run only shard K of N, 0-based: each test lands in one shard by a hash of its name, the "
                        "same on every run, and an aliased driver's children (a backend's context, say) are placed "
                        "one by one. Forwarded to the runner as --shard; the shards partition the run, which is "
                        "what a bisection needs.")
    p.add_argument("--rec", action="store_true",
                   help="Keep each binary's recording as <binary>.ccrec beside it: every instrumented scope, with no "
                        "sampling (nexus --rec). Prints where it went and how large it is.")
    p.add_argument("--stop-after", type=float, default=None, metavar="SECS",
                   help="Start no test once a binary's run has taken SECS; the tests already running finish, and the "
                        "rest are skipped, saying why. Forwarded to the runner as --stop-after.")
    p.add_argument("--sample", action="store_true",
                   help="Profile the run: each binary samples its threads until the run ends, prints where the busy "
                        "time went, and keeps the recording as <binary>.ccrec beside it. "
                        "Pair it with a test filter: the report symbolizes at exit, seconds per few thousand addresses.")
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


def _rebuild_built_outside_all(ctx: Context, args: argparse.Namespace, presets: list, primary, all_targets: list,
                               binary_names: list[str], server) -> None:
    """Builds by name each of `binary_names` that a preset's build directory already holds an artifact of."""
    selected = set(binary_names)
    for preset in presets:
        targets = all_targets if preset is primary else ctx.discover(preset, args.emsdk_path)
        names = sorted(t.name for t in targets
                       if t.name in selected and t.artifact is not None and Path(t.artifact).exists())
        if not names:
            continue
        results = dev.build(
            [preset], names, root=ctx.root,
            auto_configure=False,
            mirror=args.mirror_output, verbose=args.verbose,
            emsdk_path=args.emsdk_path, server=server,
        )
        if not all(r.ok for r in results):
            ctx.fail_build(results, [preset])


def run(args: argparse.Namespace, ctx: Context) -> None:
    presets = ctx.resolve_build_presets(args)
    primary = presets[0]
    ctx.warn_known_issues(presets)

    # parse_known_args already drops the first `--`.
    # Strip a stray leading one as well, so an explicit `test <name> -- -c foo` never leaks the separator through.
    runner_args = list(args.runner_args or [])
    if runner_args and runner_args[0] == "--":
        runner_args = runner_args[1:]

    if args.thorough:
        runner_args = ["--thorough", *runner_args]
    if args.smoke:
        runner_args = ["--smoke", *runner_args]

    # An explicit --timeout always wins, 0 included; only the default scales with --thorough.
    timeout = args.timeout
    if timeout is None:
        timeout = DEFAULT_TIMEOUT_S * (THOROUGH_TIMEOUT_FACTOR if args.thorough else 1)

    # Prepended, so an explicit `-- --jobs 4` after it still wins by being parsed later.
    if args.jobs is not None:
        runner_args = ["--jobs", str(args.jobs), *runner_args]

    if args.seed is not None:
        runner_args = ["--seed", str(args.seed), *runner_args]

    if args.shard is not None:
        runner_args = ["--shard", args.shard, *runner_args]
    if args.stop_after is not None:
        runner_args = ["--stop-after", f"{args.stop_after:g}", *runner_args]

    # The runner's own default is 60 s, which the default timeout would kill it before reaching.
    # Without --watchdog each binary gets half of ITS timeout, since a binary may declare a longer one than the default.
    if args.watchdog is not None:
        runner_args = ["--watchdog", f"{args.watchdog:g}", *runner_args]
    elif timeout <= 0:
        runner_args = ["--watchdog", "0", *runner_args]

    # One string, deliberately: the runner tokenizes it, so the test's own flags never have to survive
    # dev.py's argument handling — which strips a leading `--` and would otherwise eat the separator.
    if args.test_args is not None:
        runner_args = ["--test-args", args.test_args, *runner_args]

    # One server session for the build and the run, so it is asked once and a fallback warns once.
    server = ctx.build_server(args)

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
            emsdk_path=args.emsdk_path, server=server,
        )
        if not all(r.ok for r in results):
            ctx.fail_build(results, presets)

    # Determine which test binaries to run and the optional test-name filter.
    all_targets = ctx.discover(primary, args.emsdk_path)
    wanted = ctx.resolve_target_names(primary, args.target, args.emsdk_path) if args.target else None
    # Example binaries are candidates too: one may carry ordinary TESTs for machinery it grew, and those are tests like any other.
    # Its EXAMPLEs are in their own bucket and never run here, and a stub standing in for one carries nothing.
    binary_names, test_name, err = dev.select_test_binaries(
        all_targets,
        is_test=lambda t: ctx.is_test_target(t) or (ctx.is_example_target(t) and not ctx.is_stub_target(t)),
        wanted_names=wanted, name_arg=args.test_name, target_label=args.target,
    )
    if err:
        ctx.die(err)

    # A binary outside `all` (EXCLUDE_FROM_ALL) is not rebuilt by the build above, yet its artifact from an earlier build is still there.
    # Run or probed as it is, it is stale: a new test is missing from its listing, and a fixed one still fails.
    # So every selected binary that has an artifact is built by name, which costs a no-op ninja call when it is current.
    # One never built stays unbuilt and is reported so, rather than pulling in what `all` deliberately leaves out.
    if not args.no_build and not args.target:
        _rebuild_built_outside_all(ctx, args, presets, primary, all_targets, binary_names, server)

    # Both probes below launch the artifact, so on a wasm preset they need the same runtime the run itself will use.
    # Resolved against the emsdk overlay rather than the inherited PATH, because emsdk's bundled node is usually the
    # only one on the machine -- the setup docs say no separate Node install is needed.
    probe_launcher = jsr.LazyLauncher(jsr.JsRuntimeRequest.from_args(args), dev.emsdk_env(args.emsdk_path))

    # ...and then the example binaries that turn out to have none are dropped, so a sweep does not fail on them.
    binary_names = dev.drop_testless_examples(
        primary, all_targets, binary_names,
        is_example=ctx.is_example_target, test_name=test_name, root=ctx.root, extra_args=runner_args,
        launcher=probe_launcher,
    )

    # With a filter, only the binaries that actually contain a matching test run, queried via nexus' --list-tests-json on the primary preset.
    # A binary that cannot answer the query is kept and run as before.
    if test_name:
        binary_names, diag = dev.select_eligible_binaries(
            primary, all_targets, binary_names,
            test_name=test_name, root=ctx.root, extra_args=runner_args, launcher=probe_launcher,
        )
        if diag:
            ctx.die(diag)

    # Repeat runs the SAME selection again — build and discovery above happen once, so an iteration costs only the binaries.
    # It stops at the first failure rather than tallying a rate, so the run that failed is the one whose logs and XML are still on disk.
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
            timeout=timeout if timeout else None,
            timeout_is_explicit=args.timeout is not None,
            watchdog_fraction=0.5 if args.watchdog is None and timeout > 0 else None,
            write_xml=not args.no_xml_reports,
            mirror=args.mirror_output,
            verbose=args.verbose,
            emsdk_path=args.emsdk_path,
            runtime=jsr.JsRuntimeRequest.from_args(args),
            sample=args.sample,
            rec=args.rec,
            # A repeat chases a flake, so a stored pass must not answer for it.
            server=server, rerun=repeat > 1,
        )

        if any(r["returncode"] != 0 for r in records):
            if repeat > 1:
                print(dev.console.red(f"failed on iteration {iteration} of {repeat}"), file=sys.stderr)
            break

    if args.merged_xml_report:
        parts = [Path(r["artifact"]).parent / f"{Path(r['artifact']).name}.results.xml" for r in records]
        dev.merge_junit(parts, Path(args.merged_xml_report))

    sys.exit(0 if dev.report.summarize_tests(records, presets, ctx.root) else 1)
