# Perf Results: guide benchmarks & the `.perf.json` contract

nexus can record named performance metrics and emit them as a machine-readable sidecar.
This is the convention behind [pgo.md](pgo.md)'s training and speedup report: a small, stable set of numbers tracked across builds.
Any library, or a downstream app, can expose its own numbers the same way.
Back to [guides](_index.md).

> **This is not the benchmarking framework.**
> A guide benchmark is a *tracking signal* — a handful of representative points, watched over time and consumed by `dev.py pgo`.
> It is the wrong tool for the question "is my new implementation faster than the old one", which wants many configurations, a baseline to compare against, and a table you read once.
> Write that as a plain `nx::config::manual` test, which never runs in a sweep and may print whatever it likes —
> [sort-benchmark.cc](../../libs/base/clean-core/tests/benchmarks/sort-benchmark.cc) is the model.
> A real microbenchmark harness in nexus is still to be written; until then, `bench_util.hh`'s adaptive timers are what the existing benchmarks share.

## Writing a guide benchmark

A **guide benchmark** is a nexus test in the `guide_benchmark` bucket.
Declare it with `GUIDE_BENCHMARK` — a `TEST` that sets the bucket — and report metrics via `nx::guide`:

```cpp
#include <nexus/guide.hh>
#include <nexus/test.hh>

GUIDE_BENCHMARK("hash - throughput")
{
    double const gbps = measure(/* ... */);
    nx::guide::report_raw("xxh3@8B", gbps, "GB/s", /*higher_is_better=*/true);
}
```

`report_elements_per_sec` and `report_time_for` are shorthands that fix the unit and the orientation for you.
The [nexus cheat-sheet](../../libs/base/nexus/cheat-sheet.md) lists all three.

Each call records a `(name, value, unit, higher_is_better)` tuple onto the running test.
The orientation is what lets readers and tooling compare runs correctly, so a speedup reads as positive whether the metric is throughput or latency.
Calls are a no-op outside a running test, so guarding is never needed.
Record a **small, stable** set of representative points — one short and one long input, say — rather than an entire sweep, so deltas stay meaningful and low-noise.
Printing full tables alongside is fine, and [clean-core's benchmarks](../../libs/base/clean-core/tests/benchmarks/) do.

### Which sweep runs them

Guide benchmarks live in their own bucket, so they **never run in a normal `dev.py test` sweep**.
Not even when a substring filter matches their names: `dev.py test "hash"` leaves them alone.
Naming a test by its **exact** name runs it regardless of bucket, so `dev.py test "hash - throughput"` still works.
The bucket model itself — `normal` / `manual` / `guide_benchmark`, plus the orthogonal `disabled` — belongs to [catch2-runner-compat.md](../../libs/base/nexus/docs/catch2-runner-compat.md).

## Running and the sidecar

```bash
uv run dev.py test "hash - throughput"     # run one guide benchmark by exact name (prints the metric table)
<binary> --guide-benchmarks                # sweep every guide benchmark in a binary
<binary> --guide-benchmarks --perf-json out.perf.json   # also write the sidecar
```

Recorded metrics print as a `Recorded metrics:` table at the end of the run.
With `--perf-json <file>`, nexus also writes a sidecar, mirroring the JUnit XML mechanism.
The shape is one flat array, each entry tagged with its test:

```json
{
  "suite": "clean-core-test",
  "metrics": [
    {"test": "bench-hash (…)", "name": "xxh64@8B", "value": 2.58, "unit": "GB/s", "higher_is_better": true}
  ]
}
```

`dev.py pgo` consumes these sidecars; [pgo.md](pgo.md) covers what it does with them.
A binary with no guide benchmarks exits 0 and writes a sidecar whose `metrics` array is empty.
Only top-level tests are collected: a metric recorded inside a test dispatched by `nx::invoke_tests` reaches neither the table nor the sidecar.

## Related

- [pgo.md](pgo.md) — the profile-guided-optimization pipeline that trains on and measures these metrics.
- [nexus cheat-sheet](../../libs/base/nexus/cheat-sheet.md) — `GUIDE_BENCHMARK`, `nx::guide`, and the CLI flags.
- [catch2-runner-compat.md](../../libs/base/nexus/docs/catch2-runner-compat.md) — buckets, discovery and filtering.
- [profiling.md](profiling.md) — ad-hoc hardware counters per region, as opposed to recorded metrics over time.
- [nexus docs hub](../../libs/base/nexus/docs/_index.md) — the rest of the framework: invocable tests, fuzzing, and the runner internals.
