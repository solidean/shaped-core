"""Generic pre-commit check runner.

A `Check` is a named gate the CLI can run, and the registry of which gates exist is cmd/check.py's.
This module only knows how to *run* a selected set: static checks first, then the slow `requires_green` tail, with a colored verdict at the end.
"""

from __future__ import annotations

import sys
from collections.abc import Callable
from dataclasses import dataclass

from ..core import console, profile
from .changes import ChangeScope


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
    scope: ChangeScope | None,
    mirror: bool,
    verbose: bool,
    no_test: bool,
) -> bool:
    """Run the selected checks and print a verdict; return True if all passed.

    Static checks run first, and a `requires_green` check runs only if none of them failed and `no_test` is False.
    There is no point building and testing a tree that already fails a cheap lint.
    `scope` is handed to every check identically; a check that is always repo-wide ignores it.
    """
    failed: list[str] = []

    def run_one(c: Check) -> None:
        print(console.dim(f"\n--- running {c.name} ---"), file=sys.stderr)
        with profile.span(c.name, type="check-gate"):
            ok = c.run(fix=fix, scope=scope, mirror=mirror, verbose=verbose)
        if not ok:
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
