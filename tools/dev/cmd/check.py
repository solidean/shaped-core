"""`check` — the pre-commit aggregator: format, crossrefs, and the test gate.

Each registered `dev.Check` is a named gate. `check` with no name runs them all
(the pre-commit aggregator); `check <name>` runs a subset. A check's `run` prints
its own banner/summary and returns ok: bool. Checks that support `--fix` apply
unambiguous fixes (clang-format); others ignore it and only report. A
`requires_green` check (the test suite) is the slow tail: it runs only after every
static check passed — no point building and testing a tree that already fails a
cheap lint — and `--no-test` skips it outright (handy for a docs-only re-check).

The registry below is the project's growth point — new gates plug in here; the
generic Check type and runner live in tools/dev/lib/quality/checks.py.
"""

from __future__ import annotations

import argparse
import platform
import sys

from tools import dev
from tools.dev import console

from .context import Context

NAME = "check"


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run pre-commit checks (format, crossrefs, ...)")
    p.add_argument("names", nargs="*", help="Specific check(s) to run (default: all)")
    p.add_argument("--fix", action="store_true",
                   help="Let fixable checks apply unambiguous fixes (e.g. clang-format -i)")
    p.add_argument("--all", action="store_true",
                   help="Widen the format check from dirty-only to the whole tree")
    p.add_argument("--no-test", action="store_true",
                   help="Skip the test suite (build + run); just the static checks")
    p.add_argument("--list", action="store_true", help="List registered checks and exit")
    return p


def _build_checks(ctx: Context) -> list[dev.Check]:
    """The pre-commit registry: project policy for which gates exist."""

    def check_format(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
        # Pre-commit default is dirty-only (just the next commit's files); --all
        # widens to the whole tree. --fix rewrites in place, else check-only.
        try:
            result = dev.run_format(
                ctx.root,
                check=not fix,
                dirty_only=not all_scope,
                allow_different_version=False,
                mirror=mirror,
                verbose=verbose,
            )
        except dev.FormatSetupError as e:
            ctx.die(str(e))
        return dev.report.summarize_format(result, ctx.root)

    def check_crossrefs(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
        # Always full-repo: a moved file breaks links in other, untouched files, so a
        # dirty-only scan would miss exactly the breakage this guards against. Not
        # fixable, so fix/all_scope are ignored.
        return dev.report.summarize_crossrefs(dev.check_crossrefs(ctx.root), ctx.root)

    def check_tests(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
        # Build + run the full suite across build variants: debug (-O0 + mimalloc's
        # MI_DEBUG heap) and relwithdebinfo exercise CC_ASSERT on, release exercises
        # it off, and — where supported — a sanitizer (ASan+UBSan) preset adds a
        # memory/UB pass. Together they catch allocator misuse, assert regressions,
        # and undefined behavior the optimized presets miss. Warm builds are the norm
        # at commit time — uncompiled code couldn't have been tested — so the real
        # cost is the test run. Not fixable; fix/all_scope ignored.
        system = platform.system()
        specs = [ctx.default_preset_name()]
        for sibling in (
            ctx.policy.default_debug.get(system),
            ctx.policy.default_release.get(system),
            ctx.policy.default_sanitize.get(system),
        ):
            if sibling:
                specs.append(sibling)
        presets = ctx.resolve_presets(specs)

        results = dev.build(presets, None, root=ctx.root, auto_configure=True, mirror=mirror, verbose=verbose)
        if not all(r.ok for r in results):
            dev.report.print_build_failure(results, presets, ctx.root)
            return False

        test_targets = [t for t in ctx.discover(presets[0]) if ctx.is_test_target(t)]
        if not test_targets:
            print(console.red("No test binaries found (expected '*-test' executables)"), file=sys.stderr)
            return False

        records = dev.test(
            presets, [t.name for t in test_targets], root=ctx.root,
            test_name=None, timeout=60.0, write_xml=True, mirror=mirror, verbose=verbose,
        )
        return dev.report.summarize_tests(records, presets, ctx.root)

    return [
        dev.Check("format", "clang-format our C++ sources (dirty-only; --all for the whole tree)",
                  True, check_format),
        dev.Check("crossrefs", "validate doc<->code cross-references repo-wide", False, check_crossrefs),
        dev.Check("test",
                  "build + run the full suite on the debug, default, release (and where supported, sanitizer) presets",
                  False, check_tests, requires_green=True),
    ]


def run(args: argparse.Namespace, ctx: Context) -> None:
    checks = _build_checks(ctx)
    if args.list:
        dev.list_checks(checks)
        sys.exit(0)

    by_name = {c.name: c for c in checks}
    if args.names:
        selected: list[dev.Check] = []
        seen: set[str] = set()
        for name in args.names:
            if name not in by_name:
                ctx.die(f"unknown check {name!r}. Available: {', '.join(by_name)}")
            if name not in seen:
                seen.add(name)
                selected.append(by_name[name])
    else:
        selected = list(checks)

    ok = dev.run_checks(
        selected, fix=args.fix, all_scope=args.all,
        mirror=args.mirror_output, verbose=args.verbose, no_test=args.no_test,
    )
    sys.exit(0 if ok else 1)
