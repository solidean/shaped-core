#!/usr/bin/env -S uv run
# /// script
# requires-python = ">=3.10"
# ///

"""Self-test for benchmark selection — tools/dev/lib/pipeline/benchmarks.py.

A manual benchmark runs in nexus only when named exactly, so selection has to agree:
an exact name reaches it, a substring passes it over, and it never counts as a sibling a substring would also run.

    uv run tools/dev/benchmarks-self-test.py
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.dev.lib.pipeline.benchmarks import Benchmark, select_benchmarks, unselected_overreach  # noqa: E402


def bench(name: str, manual: bool = False) -> Benchmark:
    return Benchmark(name=name, target="x-test", file="x.cc", line=1, manual=manual)


def names(benchmarks: list[Benchmark]) -> list[str]:
    return sorted(b.name for b in benchmarks)


def main() -> int:
    quick = bench("solver bench - quick")
    sweep = bench("solver bench - quick, swept slowly", manual=True)
    corpus = [quick, sweep]
    failures: list[str] = []
    checks = 0

    def check(what: str, ok: bool) -> None:
        nonlocal checks
        checks += 1
        if not ok:
            failures.append(what)

    selected, _ = select_benchmarks(corpus, "solver bench")
    check("a substring selects the plain benchmark and passes the manual one over", names(selected) == [quick.name])

    selected, _ = select_benchmarks(corpus, sweep.name)
    check("the exact name selects the manual benchmark", names(selected) == [sweep.name])

    selected, message = select_benchmarks(corpus, "swept slowly")
    check("a substring hitting only a manual benchmark selects nothing", selected == [])
    check("and says to name it exactly", message is not None and sweep.name in message)

    selected, _ = select_benchmarks(corpus, quick.name)
    check("a manual sibling the exact name is a substring of is no overreach", unselected_overreach(selected, corpus) == [])

    plain_sibling = bench("solver bench - quick, swept")
    selected, _ = select_benchmarks([quick, plain_sibling], quick.name)
    check("a plain sibling still is", unselected_overreach(selected, [quick, plain_sibling]) == [(quick.name, plain_sibling.name)])

    for f in failures:
        print(f"FAIL: {f}")
    print(f"benchmarks-self-test: {checks - len(failures)} of {checks} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
