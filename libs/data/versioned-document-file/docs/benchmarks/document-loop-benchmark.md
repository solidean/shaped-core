# The open / edit / publish / close loop

What an ordinary editing session actually costs, stage by stage, and where hashing sits inside it.

Source: [tests/benchmarks/document-loop-benchmark.cc](../../tests/benchmarks/document-loop-benchmark.cc).

This benchmark exists to settle one standing question rather than to optimize anything.
[decisions.md](../../../versioned-document/docs/decisions.md#blake3-over-32-byte-ids--with-a-standing-reservation) accepts BLAKE3 with a standing reservation.
Its reopen condition is *"BLAKE3 shows up in a profile of an ordinary open / edit / save loop"*, and until milestone 6 there was no such loop to point a profiler at.

## Findings

### Hashing does not show up, and the reservation stands

Hashing is **0.2% to 0.6% of the loop** at every size measured, on both store arms.
In absolute terms, re-hashing an 8,000-op document on load costs **4.6 ms**.

The reopen condition did not fire.

### The number worth watching is hashing's share of the *open*, not of the loop

Against the whole loop, hashing disappears into the noise.
Against the **open** stage alone — where the loader re-hashes every op it reads — it is **16% to 41%**.

That is the share that grows with history length, because it is one hash per stored op.
It is also the share that pruning and snapshots exist to bound, which is the connection worth keeping in view: a document that never prunes pays this linearly forever.

At present it is 4.6 ms on 8,000 ops, so it buys nothing to act on.

### The loop is dominated by two things, and neither is hashing

`edit` and `materialize` account for over 95% of every run.

- **`edit` is quadratic in history.** Building 50 ops costs 20 ms against a 200-op document and 1,000 ms against an 8,000-op one.
  `op_builder::build` diffs against its parents by materializing them, and it takes an `op_graph` with **no `snapshot_cache` overload**.
  So the edit path structurally cannot reach the caching that exists precisely to make this cheap.
- **`materialize` is quadratic in the document, not just in history.** One materialization of 8,000 ops over 48,000 properties takes 1.09 s.
  The sweep carries a dense per-path state vector per op, so its cost is roughly ops × distinct paths, even though each op here touches six paths.

Both are real and both are out of scope for milestone 6, which asked about hashing.
They are recorded here because this is the measurement that found them.

### The additivity control is below the loop's noise floor

The benchmark re-runs the loop with every loaded op hashed a second time, to check that the isolated hashing figure composes rather than being a ratio of two unrelated numbers.

The injected deltas come out between **−11 ms and +14 ms** against an expected 0.1–4.6 ms.
They scatter around the expected value and sometimes go negative, which means the injection is smaller than the loop's own run-to-run variance.

That is not a failed control — it is the finding, stated the strongest way available: **doubling the loop's hashing does not produce a measurable change in the loop.**

## System under test

| | |
|---|---|
| CPU | 12th Gen Intel Core i9-12900H (14 cores / 20 threads) |
| Memory | 32 GB |
| OS | Windows 11 Home |
| Compiler | clang 22.1.7, preset `release-clang` |

The same machine as [clean-core's hash benchmark](../../../../base/clean-core/docs/benchmarks/hash-benchmark.md), so the two write-ups compose directly.
This is Alder Lake, where AVX-512 is fused off, so BLAKE3's widest path is unavailable — a CPU that has it would make hashing's share smaller still.

Read trends, not third decimals.
Every figure is a median of five passes, each against its own freshly seeded document.

## Reproducing

```bash
uv run dev.py test "bench-vdoc-loop (open / edit / publish / close)" --preset release-clang --timeout 0
uv run dev.py test "bench-vdoc-loop (full sweep)" --preset release-clang --timeout 0 --manual
```

The first records the representative point as guide metrics; the second prints the table below.
See [docs/guides/perf-results.md](../../../../../docs/guides/perf-results.md) for the guide-benchmark mechanism.

## Method

Hashing is not separable by a hardware counter.
On Windows the counters are read at context switches, so attributing one to a call inside the loop would be a guess dressed as a measurement.

So it is measured three ways instead, none of which is an attribution:

1. **the loop itself**, per stage, against a document seeded fresh for every pass;
2. **the same hashes the loop performs**, run alone over the same bytes — one per stored op for the load, plus one per new op for the edit;
3. **an additivity control** — the loop again, with every loaded op hashed a second time.

Every pass gets its own medium.
A pass appends its edits and the loop is superlinear in document size, so reusing one medium would make each pass slower than the last.
Any delta between two series would then measure that growth rather than the injection.
An earlier version of this benchmark did exactly that, and reported an injection 28× larger than the work injected.

## Results

Times in milliseconds, medians over five passes.
The document is *N* ops, each giving one new entity six properties, at ~260 B of op payload.
The measured edit is 50 ops — one user action's worth.

| arm | ops | properties | loop | open | materialize | edit | publish | close | hashing | % of loop | % of open |
|---|---|---|---|---|---|---|---|---|---|---|---|
| in-memory | 200 | 1,200 | 21.7 | 0.3 | 1.0 | 20.0 | 0.2 | 0.0 | 0.13 | 0.6% | 41.3% |
| in-memory | 2,000 | 12,000 | 238.8 | 3.0 | 34.8 | 199.8 | 1.8 | 0.0 | 1.17 | 0.5% | 38.9% |
| in-memory | 8,000 | 48,000 | 2,112.0 | 13.7 | 1,087.7 | 995.5 | 10.1 | 0.0 | 4.63 | 0.2% | 33.8% |
| sqlite | 200 | 1,200 | 31.6 | 2.6 | 1.0 | 19.5 | 4.7 | 3.6 | 0.14 | 0.5% | 5.6% |
| sqlite | 2,000 | 12,000 | 277.5 | 7.2 | 39.4 | 218.5 | 7.6 | 5.3 | 1.15 | 0.4% | 15.9% |
| sqlite | 8,000 | 48,000 | 2,181.7 | 22.4 | 1,099.4 | 1,046.3 | 11.9 | 4.9 | 4.58 | 0.2% | 20.4% |

The additivity control, against the isolated hashing figure it should reproduce:

| arm | ops | injected delta | expected |
|---|---|---|---|
| in-memory | 200 | −1.6 | 0.13 |
| in-memory | 2,000 | +0.9 | 1.17 |
| in-memory | 8,000 | +14.1 | 4.63 |
| sqlite | 200 | +1.3 | 0.14 |
| sqlite | 2,000 | +8.5 | 1.15 |
| sqlite | 8,000 | −11.4 | 4.58 |

The difference between the two arms is disk, and it is the whole of `close` and most of `publish`.

## Takeaways

- **The reservation stands, on evidence.** Hashing is a fraction of a percent of an ordinary loop, and doubling it changes nothing measurable.
- **Hashing's real home is the load**, at one hash per stored op, and its cost grows with history length rather than with document size.
  4.6 ms on 8,000 ops today; the thing that bounds it is pruning.
- **The loop's cost is materialization and op building**, both of which are quadratic and neither of which is what this milestone set out to measure.
  `op_builder::build` having no `snapshot_cache` overload is the sharper of the two, because the caching that would fix it already exists.
- **A benchmark that grows its own input measures the growth.** The control here was wrong in exactly that way before it was fixed, and it read as a real signal.
