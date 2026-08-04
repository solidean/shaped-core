"""`check` — the project's pre-commit registry: which gates exist, and in what order they run.

The generic `Check` type and the runner are tools/dev/lib/quality/checks.py; this file is the list, and the growth point for a new gate.
docs/guides/building-and-testing.md documents what each gate does, and the argument behind the ordering.
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
    p = sub.add_parser(NAME, help="Run the pre-commit checks, in registry order (lint, format, crossrefs, test)")
    p.add_argument("names", nargs="*", help="Specific check(s) to run (default: all)")
    p.add_argument("--fix", action="store_true",
                   help="Let fixable checks apply unambiguous fixes (e.g. clang-format -i)")
    p.add_argument("--all", action="store_true",
                   help="Widen lint, shaped-lint and format from dirty-only to the whole tree")
    p.add_argument("--no-test", action="store_true",
                   help="Skip the test suite (build + run); just the static checks")
    p.add_argument("--list", action="store_true", help="List registered checks and exit")
    return p


def _build_checks(ctx: Context) -> list[dev.Check]:
    """The pre-commit registry: project policy for which gates exist."""

    def check_format(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
        # --fix rewrites in place; without it clang-format only reports.
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

    def check_lint(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
        from .lint import run_clang_tidy
        return run_clang_tidy(
            ctx, preset_specs=None, dirty_only=not all_scope, fix=fix, mirror=mirror, verbose=verbose,
        )

    def check_shaped_lint(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
        from .lint import run_shaped_linter
        return run_shaped_linter(
            ctx, preset_specs=None, dirty_only=not all_scope, fix=fix, mirror=mirror, verbose=verbose,
        )

    def check_crossrefs(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
        # Not fixable and never dirty-only, so fix and all_scope are both ignored.
        return dev.report.summarize_crossrefs(dev.check_crossrefs(ctx.root), ctx.root)

    def check_tests(*, fix: bool, all_scope: bool, mirror: bool, verbose: bool) -> bool:
        # The variants come from dev.py's Policy tables, and a platform with no sibling for one of them simply contributes none.
        # Not fixable, so fix and all_scope are ignored.
        system = platform.system()
        specs = [ctx.default_preset_name()]
        for sibling in (
            ctx.policy.default_debug.get(system),
            ctx.policy.default_release.get(system),
            ctx.policy.default_singlethreaded.get(system),
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

    # ORDER IS LOAD-BEARING: every fixing gate runs before `format`, so what they rewrite is formatted in the same pass.
    # docs/guides/building-and-testing.md has the argument.
    return [
        dev.Check("lint", "clang-tidy gates on the next commit's C++ (dirty-only; --all for the whole tree)",
                  True, check_lint),
        dev.Check("shaped-lint", "shaped-linter's own rules on the next commit's code and prose "
                                 "(dirty-only; --all for the whole tree)",
                  True, check_shaped_lint),
        dev.Check("format", "clang-format our C++ sources, last so it formats what the linters fixed "
                            "(dirty-only; --all for the whole tree)",
                  True, check_format),
        dev.Check("crossrefs", "validate doc<->code cross-references repo-wide", False, check_crossrefs),
        dev.Check("test",
                  "build + run the full suite on the debug, default, release, single-threaded "
                  "(and where supported, sanitizer) presets",
                  False, check_tests, requires_green=True),
    ]


def run(args: argparse.Namespace, ctx: Context) -> None:
    checks = _build_checks(ctx)
    if args.list:
        dev.list_checks(checks)
        sys.exit(0)

    by_name = {c.name: c for c in checks}
    if args.names:
        for name in args.names:
            if name not in by_name:
                ctx.die(f"unknown check {name!r}. Available: {', '.join(by_name)}")
        # Registry order, not the order the names were typed.
        # The sequence is a correctness property, so `check format lint --fix` must not silently undo it.
        wanted = set(args.names)
        selected = [c for c in checks if c.name in wanted]
    else:
        selected = list(checks)

    ok = dev.run_checks(
        selected, fix=args.fix, all_scope=args.all,
        mirror=args.mirror_output, verbose=args.verbose, no_test=args.no_test,
    )
    sys.exit(0 if ok else 1)
