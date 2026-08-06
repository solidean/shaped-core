# nexus docs

Documentation hub for nexus, the `nx` test framework.
For the library overview and the source map, start at the [readme](../readme.md).
For the API at a glance — every macro, the check vocabulary, the gotchas — start at the [cheat-sheet](../cheat-sheet.md).
Repo-wide docs are at [docs/_index.md](../../../../docs/_index.md).

## Topics

- [invocable-tests](invocable-tests.md) — `INVOCABLE_TEST` + `nx::invoke_tests`, the one mechanism behind parametrized, data-driven and generator tests.
  Covers signature matching, addressing a single instance, aliases, and the orphan check.
- [fuzz-testing](fuzz-testing.md) — `nx::fuzz`, the API-sequence fuzzer: typed operations, invariants, shrinking, and emitted regression code.
  The shared-state section is required reading before fuzzing over a GPU context or any other resource your operations close over.
- [catch2-runner-compat](catch2-runner-compat.md) — the CLI layer: which Catch2 v3 flags nexus accepts, how IDE discovery works, buckets and filters, and the JUnit and JSON side-outputs.
- [stdlib-migration](stdlib-migration.md) — the remaining `std::` usages and what each is waiting on, so the move onto clean-core can finish as clean-core grows.

## Elsewhere

Two nexus capabilities are documented at repo level, next to the `dev.py` workflow that consumes them:

- [docs/guides/perf-results](../../../../docs/guides/perf-results.md) — `GUIDE_BENCHMARK` + `nx::guide`, and how a recorded metric reaches `dev.py pgo`.
- [docs/guides/profiling](../../../../docs/guides/profiling.md) — `nx::bench::measure_hw_counters`, what each platform can measure, and the setup a non-elevated Windows user needs.

## Conventions

Code follows the repo [coding-guidelines](../../../../docs/coding-guidelines.md), and `.clang-format` is authoritative for formatting.
