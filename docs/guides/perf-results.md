# Perf Results: guide benchmarks & the `.perf.json` contract

nexus can record named performance metrics from a benchmark and emit them as a
machine-readable sidecar. This is the reusable convention behind
[pgo.md](pgo.md)'s training and speedup report, but it stands on its own: any
library (or downstream app) can expose stable, comparable performance numbers
this way.

## Writing a guide benchmark

A **guide benchmark** is a nexus test in the `guide_benchmark` bucket. Declare it
with `GUIDE_BENCHMARK` (a `TEST` that sets the bucket) and report metrics via
`nx::guide`:

```cpp
#include <nexus/guide.hh>
#include <nexus/test.hh>

GUIDE_BENCHMARK("hash - throughput")
{
    double const gbps = measure(/* ... */);

    nx::guide::report_raw("xxh3@8B", gbps, "GB/s", /*higher_is_better=*/true); // explicit unit + orientation
    nx::guide::report_elements_per_sec("keys", keys_per_second);              // unit "1/s", higher is better
    nx::guide::report_time_for("op", seconds);                               // unit "s",   lower  is better
}
```

Each call records a `(name, value, unit, higher_is_better)` tuple onto the running
test. The orientation lets readers and tooling compare runs correctly — a speedup
reads as positive whether the metric is throughput or latency. Calls are a no-op
outside a running test, so guarding is never needed. Record a **small, stable** set
of representative points (e.g. one short and one long input), not an entire sweep,
so deltas stay meaningful and low-noise; keep printing full tables alongside if you
like (see [clean-core's benchmarks](../../libs/base/clean-core/tests/benchmarks/)).

### Buckets

Every nexus test is in exactly one bucket — `normal` (default), `manual`, or
`guide_benchmark`. A sweep selects one bucket: the default run sweeps `normal`,
`--manual` sweeps `manual`, and `--guide-benchmarks` sweeps `guide_benchmark`.
Guide benchmarks therefore never run in a normal `dev.py test` sweep — not even
when a substring filter matches their names, so `dev.py test "hash"` leaves them
alone. `disabled` is orthogonal and can apply to any bucket. Naming a test by its
**exact** name runs it regardless of bucket, so `dev.py test "hash - throughput"`
still works.

## Running and the sidecar

```bash
uv run dev.py test "hash - throughput"     # run one guide benchmark by exact name (prints the metric table)
<binary> --guide-benchmarks                # sweep every guide benchmark in a binary
<binary> --guide-benchmarks --perf-json out.perf.json   # also write the sidecar
```

Recorded metrics print as a `Recorded metrics:` table at the end of the run. With
`--perf-json <file>`, nexus also writes a sidecar — mirroring the JUnit XML
mechanism. The shape is one flat array, each entry tagged with its test:

```json
{
  "suite": "clean-core-test",
  "metrics": [
    {"test": "bench-hash (…)", "name": "xxh64@8B", "value": 2.58, "unit": "GB/s", "higher_is_better": true}
  ]
}
```

`dev.py pgo` consumes these: it runs `--guide-benchmarks` across every `*-test`
binary, parses each `.perf.json`, and diffs baseline vs PGO by
`(binary, test, name)`. A binary with no guide benchmarks exits 0 and writes no
sidecar.

## Related

- [pgo.md](pgo.md) — the profile-guided-optimization pipeline that trains on and measures these metrics.
- [nexus cheat-sheet](../../libs/base/nexus/cheat-sheet.md) — `GUIDE_BENCHMARK`, `nx::guide`, and the CLI flags.
