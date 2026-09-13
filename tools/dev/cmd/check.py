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

from . import args as a
from .context import Context

NAME = "check"

# What one preset's whole test run is expected to stay inside.
#
# 120 s, which every preset currently clears — so a green run is green rather than green-with-a-warning.
# The suite is slower than it should be and that is tracked separately; this budget is the ceiling a run must not
# cross, not the target it should hit.
# Tighten it as the suite gets faster; raising it to silence a warning is how this stops meaning anything.
_SLOW_TEST_BUDGET_S = 120.0


def add_parser(sub: argparse._SubParsersAction) -> argparse.ArgumentParser:
    p = sub.add_parser(NAME, help="Run the pre-commit checks, in registry order (lint, format, crossrefs, test)")
    a.profile(p)
    p.add_argument("names", nargs="*", help="Specific check(s) to run (default: all)")
    p.add_argument("--fix", action="store_true",
                   help="Let fixable checks apply unambiguous fixes (e.g. clang-format -i)")
    a.change_scope(p, default_all=False)
    p.add_argument("--no-test", action="store_true",
                   help="Skip the test suite (build + run); just the static checks")
    p.add_argument("--list", action="store_true", help="List registered checks and exit")
    return p


def _branch_base(ctx: Context) -> str | None:
    """The merge base with the default branch, or None when there is no sensible one.

    None falls the default back to the working tree, which is what a detached checkout, a shallow clone or a tree
    already ON the default branch should get — on `main` itself "changed since main" is empty, and a gate that
    inspects nothing is worse than one that inspects the dirty files.
    """
    import subprocess

    for base in ("origin/main", "main"):
        try:
            head = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=str(ctx.root),
                                  capture_output=True, text=True, timeout=30)
            if head.returncode == 0 and head.stdout.strip() in ("main", "HEAD"):
                return None
            out = subprocess.run(["git", "merge-base", base, "HEAD"], cwd=str(ctx.root),
                                 capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError):
            return None
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    return None


def _build_checks(ctx: Context) -> list[dev.Check]:
    """The pre-commit registry: project policy for which gates exist."""

    def check_format(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # --fix rewrites in place; without it clang-format only reports.
        try:
            result = dev.run_format(
                ctx.root,
                check=not fix,
                scope=scope,
                allow_different_version=False,
                mirror=mirror,
                verbose=verbose,
            )
        except dev.FormatSetupError as e:
            ctx.die(str(e))
        return dev.report.summarize_format(result, ctx.root)

    def check_lint(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        from .lint import run_clang_tidy
        return run_clang_tidy(
            ctx, preset_specs=None, scope=scope, fix=fix, mirror=mirror, verbose=verbose,
        )

    def check_shaped_lint(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        from .lint import run_shaped_linter
        return run_shaped_linter(
            ctx, preset_specs=None, scope=scope, fix=fix, mirror=mirror, verbose=verbose,
        )

    def check_crossrefs(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # A moved file breaks links in other, untouched files, so this is always repo-wide: fix and scope are both ignored.
        return dev.report.summarize_crossrefs(dev.check_crossrefs(ctx.root), ctx.root)

    def check_deps_licenses(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # A dependency's license is a property of the manifest set, not of the next commit's files, so this is repo-wide too.
        # `--check` writes nothing and reaches no network, which is what makes it cheap enough to sit here; fix and scope are ignored.
        from .deps import run_licenses

        return run_licenses(ctx, check=True, verbose=verbose)

    def check_review(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # The review tool's own suite: coverage math, change identity and the entry grammar.
        # It builds throwaway git repositories and reaches no network, so it is cheap enough to sit here.
        # Not fixable and not scopable, so fix and scope are ignored.
        runner = ctx.root / "tools" / "review" / "review-self-test.py"
        result = dev.run_step(
            ["uv", "run", str(runner)],
            step_type="review", name="review-self-test",
            build_dir=ctx.root / "build", cwd=ctx.root, mirror=mirror, verbose=verbose,
        )
        return result.ok

    def check_dev_selftest(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # dev.py's own machinery, which nothing else exercises: the job profile's layout, the change-scope resolver, and
        # the live progress region.
        # The region is the one that most needs it — it exists only as escape sequences on a stream, so a frame that
        # miscounts its own height has no other way of being caught.
        # Not fixable and not scopable, so fix and scope are ignored.
        ok = True
        for runner in ("profile-self-test.py", "changes-self-test.py", "ui-self-test.py"):
            result = dev.run_step(
                ["uv", "run", str(ctx.root / "tools" / "dev" / runner)],
                step_type="selftest", name=runner.removesuffix(".py"),
                build_dir=ctx.root / "build", cwd=ctx.root, mirror=mirror, verbose=verbose,
            )
            ok = ok and result.ok
        return ok

    def check_tests(*, fix: bool, scope: dev.ChangeScope | None, mirror: bool, verbose: bool) -> bool:
        # The variants come from dev.py's Policy tables, and a platform with no sibling for one of them simply contributes none.
        # Not fixable, so fix and scope are ignored.
        system = platform.system()
        specs = [ctx.default_preset_name()]
        for sibling in (
            ctx.policy.default_debug.get(system),
            ctx.policy.default_release.get(system),
            ctx.policy.default_singlethreaded.get(system),
            ctx.policy.default_sanitize.get(system),
            ctx.policy.default_sanitize_thread.get(system),
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
        ok = dev.report.summarize_tests(records, presets, ctx.root)

        # After the verdict, and never part of it: this is a measurement of the run that just happened.
        # `check` is the longest command anyone here runs, and a single total cannot say which preset or which half.
        dev.report.summarize_check_timing(presets, results, records, slow_test_s=_SLOW_TEST_BUDGET_S)
        return ok

    # ORDER IS LOAD-BEARING: every fixing gate runs before `format`, so what they rewrite is formatted in the same pass.
    # docs/guides/building-and-testing.md has the argument.
    return [
        dev.Check("lint", "clang-tidy gates on what this branch changed (--dirty-only, --commit or --all to rescope)",
                  True, check_lint),
        dev.Check("shaped-lint", "shaped-linter's own rules on what this branch changed in code and prose "
                                 "(--dirty-only, --commit or --all to rescope)",
                  True, check_shaped_lint),
        dev.Check("format", "clang-format our C++ sources, last so it formats what the linters fixed "
                            "(--dirty-only, --commit or --all to rescope)",
                  True, check_format),
        dev.Check("crossrefs", "validate doc<->code cross-references repo-wide", False, check_crossrefs),
        dev.Check("deps-licenses", "verify docs/licenses/ matches the extern/ manifests, and each license is on the allowlist",
                  False, check_deps_licenses),
        dev.Check("dev-selftest", "dev.py's own self-tests (job profile, change scope, progress region)",
                  False, check_dev_selftest),
        dev.Check("review", "run the review tool's own suite (coverage math, change identity, the entry grammar)",
                  False, check_review),
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

    # --all is the whole tree, --commit is that diff, and the default is the working tree.
    # Default: everything this branch changed against the default branch, working tree included.
    #
    # A gate scoped to the working tree alone empties as you commit, so a run right before pushing — which is when it
    # matters most — would inspect nothing and report green.
    # `--dirty-only` is still there for the tight edit loop, and `--all` for the whole tree.
    scope = a.scope_from_args(args, default_since=_branch_base(ctx))
    if scope is not None and scope.revision is not None:
        # Fail on a bad revision here, rather than once a gate is already running.
        try:
            dev.resolve_range(ctx.root, scope.revision)
        except dev.ChangeScopeError as e:
            ctx.die(str(e))
    if scope is not None and scope.since is not None:
        # Same, for the branch-base kind: changed_files raises on an unresolvable base, and a gate is a bad place to
        # discover that.
        try:
            dev.changed_files(ctx.root, scope)
        except dev.ChangeScopeError as e:
            ctx.die(str(e))

    ok = dev.run_checks(
        selected, fix=args.fix, scope=scope,
        mirror=args.mirror_output, verbose=args.verbose, no_test=args.no_test,
    )
    sys.exit(0 if ok else 1)
