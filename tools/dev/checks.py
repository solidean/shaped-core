"""Generic pre-commit check runner.

A `Check` is a named gate the CLI can run; the registry of which checks exist
(and the project policy each enforces) lives in dev.py. This module only knows
how to *run* a selected set: static checks first, then the slow `requires_green`
tail (only if every static check passed and tests weren't skipped), with a
colored verdict at the end.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from dataclasses import dataclass

from . import console


@dataclass
class Check:
    name: str
    description: str
    supports_fix: bool
    run: Callable[..., bool]
    # The slow tail: run only after every static check is green (see run_checks).
    requires_green: bool = False


def list_checks(checks: list[Check]) -> None:
    """Print one line per registered check, tagging --fix / needs-green support."""
    for c in checks:
        tags = []
        if c.supports_fix:
            tags.append("--fix")
        if c.requires_green:
            tags.append("needs-green")
        suffix = f"  [{', '.join(tags)}]" if tags else ""
        print(f"{c.name}{suffix}  {c.description}")


def run_checks(
    selected: list[Check],
    *,
    fix: bool,
    all_scope: bool,
    mirror: bool,
    verbose: bool,
    no_test: bool,
) -> bool:
    """Run the selected checks and print a verdict; return True if all passed.

    Static checks run first; each `requires_green` check (the test suite) runs
    only if no static check failed and `no_test` is False — there's no point
    building and testing a tree that already fails a cheap lint.
    """
    failed: list[str] = []

    def run_one(c: Check) -> None:
        print(console.dim(f"\n--- running {c.name} ---"), file=sys.stderr)
        if not c.run(fix=fix, all_scope=all_scope, mirror=mirror, verbose=verbose):
            failed.append(c.name)

    for c in selected:
        if not c.requires_green:
            run_one(c)
    for c in selected:
        if not c.requires_green:
            continue
        if no_test:
            print(console.yellow(f"\n--- skipped {c.name} (--no-test) ---"), file=sys.stderr)
        elif failed:
            print(console.yellow(f"\n--- skipped {c.name} (static checks failed) ---"), file=sys.stderr)
        else:
            run_one(c)

    if failed:
        print(console.red("\ncheck: FAIL"), file=sys.stderr)
        for name in failed:
            print(console.red(f"  - {name}"), file=sys.stderr)
        return False
    print(console.green("\ncheck: OK"), file=sys.stderr)
    return True
