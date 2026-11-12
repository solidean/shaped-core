#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.12"
# ///

"""Self-test for tools/dev/lib/quality/checks.py — the order `run_checks` runs a gate selection in.

A build-free gate that fails must stop the run before the build, so a cheap check's verdict costs seconds.
Nothing else exercises it, and a regression only shows as a slow red gate, never as a wrong verdict.

Kept out of `dev.py test`, which drives the C++ nexus suites; this is Python tooling testing Python tooling.

    uv run tools/dev/checks-self-test.py [-v]
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.lib.quality.checks import Check, run_checks  # noqa: E402

VERBOSE = "-v" in sys.argv


def gate(log: list[str], name: str, *, ok: bool = True, builds: tuple[str, ...] = (), fix: bool = False) -> Check:
    def run(**_: object) -> bool:
        log.append(name)
        return ok

    return Check(name=name, description=name, supports_fix=fix, run=run, always=True, builds=builds)


def selection(log: list[str], *, cheap_ok: bool) -> tuple[list[Check], callable]:
    checks = [
        gate(log, "cheap-a"),
        gate(log, "cheap-b", ok=cheap_ok),
        gate(log, "needs-build", builds=("target",)),
        gate(log, "fixer", fix=True),
    ]

    def build(targets: list[str]) -> bool:
        log.append("build:" + ",".join(targets))
        return True

    return checks, build


def run(checks: list[Check], build: callable) -> bool:
    return run_checks(checks, fix=True, scope=None, mirror=False, verbose=False, no_test=True, build=build)


def test_a_failing_build_free_gate_stops_before_the_build() -> None:
    log: list[str] = []
    checks, build = selection(log, cheap_ok=False)
    assert not run(checks, build)
    assert sorted(log) == ["cheap-a", "cheap-b"], log


def test_a_green_run_builds_after_the_build_free_gates_and_fixes_last() -> None:
    log: list[str] = []
    checks, build = selection(log, cheap_ok=True)
    assert run(checks, build)
    assert sorted(log[:2]) == ["cheap-a", "cheap-b"], log
    assert log[2:] == ["build:target", "needs-build", "fixer"], log


def main() -> int:
    tests = [v for k, v in globals().items() if k.startswith("test_")]
    failures = 0
    for t in tests:
        try:
            t()
            if VERBOSE:
                print(f"ok   {t.__name__}")
        except AssertionError as e:
            failures += 1
            print(f"FAIL {t.__name__}: {e}")
    print(f"checks-self-test: {len(tests) - failures} of {len(tests)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
