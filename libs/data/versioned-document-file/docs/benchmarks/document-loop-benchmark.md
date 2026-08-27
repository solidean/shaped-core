# The open / edit / publish / close loop

What an ordinary editing session actually costs, stage by stage, and where hashing sits inside it.

Source: [tests/benchmarks/document-loop-benchmark.cc](../../tests/benchmarks/document-loop-benchmark.cc).

This benchmark exists to settle one standing question rather than to optimize anything.
[decisions.md](../../../versioned-document/docs/decisions.md#blake3-over-32-byte-ids--with-a-standing-reservation) accepts BLAKE3 with a standing reservation.
Its reopen condition is *"BLAKE3 shows up in a profile of an ordinary open / edit / save loop"*, and until milestone 6 there was no such loop to point a profiler at.

## Findings

### Hashing does not show up, and the reservation stands

Hashing was **0.2% to 0.6% of the loop** when this was first measured, and is **1% to 7%** now.
In absolute terms it has not moved at all: re-hashing an 8,000-op document on load costs **5 ms**, then and now.

The reopen condition did not fire, and the reason the share moved is worth stating plainly.
The loop got 26x faster — 2,182 ms to 85 ms at 8,000 ops — so a fixed cost became a larger fraction of a much smaller number.
That is why the share of the *open* below is the one to watch instead: it is the share that is actually about hashing.

### The number worth watching is hashing's share of the *open*, not of the loop

Against the whole loop, hashing disappears into the noise.
Against the **open** stage alone — where the loader re-hashes every op it reads — it is **21% to 42%**.

That is the share that grows with history length, because it is one hash per stored op.
It is also the share that pruning and snapshots exist to bound, which is the connection worth keeping in view: a document that never prunes pays this linearly forever.

At present it is 4.6 ms on 8,000 ops, so it buys nothing to act on.

### The loop was dominated by two things, and both are gone

`edit` and `materialize` accounted for over 95% of every run when this was first measured, and neither was hashing.
Both were fixed in milestone 7, and this is what moved:

| stage, 8,000 ops | was | is |
|---|---|---|
| `materialize` | 1,099 ms | 43 ms |
| `edit`, 50 ops | 1,046 ms | 2 ms |
| whole loop | 2,182 ms | 85 ms |

- **`edit` was quadratic in history**, because `op_builder::build` diffs against its parents by materializing them and had no `snapshot_cache` overload.
  So the edit path structurally could not reach the caching built to make exactly this cheap; it has one now.
- **`materialize` was quadratic in ops × paths**, for a reason this write-up originally got wrong.

What is left in the loop is `open` and `materialize`, and both are inherent to the question being asked: reading and hashing every stored op, then building the whole nested `raw_document`.
An application that only wants to know *what changed* asks a different question.
That is the [edit-latency benchmark](../../../versioned-document/docs/benchmarks/edit-latency-benchmark.md), where the same edit costs microseconds.

#### What `materialize` was actually spending it on

This write-up originally attributed the cost to "a dense per-path state vector per op", and **that was wrong**.
The sweep steals its parent's state when it is the sole consumer, so a linear history — which this document is — copies nothing.
The number did not fit the explanation either: 4× the ops cost 31×, where ops × paths predicts 16×.

The real cost was the **articulation-point clear**, which walked every path slot on every op.
In a linear history every op is an articulation point, so that is ops × paths: 8,000 × 48,000 ≈ 384 million iterations, which matches the 1.09 s almost exactly.
The clear now walks a dirty list instead — see [decisions.md](../../../versioned-document/docs/decisions.md#the-sweeps-per-path-state-carries-side-lists-so-nothing-walks-the-whole-path-set-per-op).

The generalizable part is that the two mechanisms are indistinguishable from the shape of the curve, and only one of them survives reading the code.

### The additivity control is below the loop's noise floor

The benchmark re-runs the loop with every loaded op hashed a second time, to check that the isolated hashing figure composes rather than being a ratio of two unrelated numbers.

The injected deltas come out between **−6.8 ms and +3.5 ms** against an expected 0.1–5.0 ms.
They scatter around the expected value and often go negative, which means the injection is smaller than the loop's own run-to-run variance.

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
See [docs/guides/perf-results.md](../../../../../docs/guides/perf-results.md) for the pgo-benchmark mechanism.

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
| in-memory | 200 | 1,200 | 3.9 | 0.4 | 0.9 | 2.4 | 0.2 | 0.0 | 0.13 | 3.3% | 36.8% |
| in-memory | 2,000 | 12,000 | 19.8 | 3.8 | 11.2 | 2.6 | 2.0 | 0.0 | 1.33 | 6.7% | 35.0% |
| in-memory | 8,000 | 48,000 | 71.8 | 15.7 | 45.7 | 2.3 | 7.4 | 0.0 | 5.03 | 7.0% | 32.1% |
| sqlite | 200 | 1,200 | 14.9 | 2.3 | 0.9 | 2.1 | 5.3 | 3.8 | 0.14 | 1.0% | 6.2% |
| sqlite | 2,000 | 12,000 | 31.9 | 7.0 | 9.0 | 2.4 | 7.1 | 5.6 | 1.41 | 4.4% | 20.0% |
| sqlite | 8,000 | 48,000 | 84.9 | 22.1 | 43.2 | 2.2 | 11.6 | 5.8 | 4.95 | 5.8% | 22.4% |

For reference, the same table before the articulation-point clear was fixed — the row that moved is `materialize`:

| arm | ops | loop | materialize | edit |
|---|---|---|---|---|
| sqlite | 200 | 31.6 | 1.0 | 19.5 |
| sqlite | 2,000 | 277.5 | 39.4 | 218.5 |
| sqlite | 8,000 | 2,181.7 | 1,099.4 | 1,046.3 |

The additivity control, against the isolated hashing figure it should reproduce:

| arm | ops | injected delta | expected |
|---|---|---|---|
| in-memory | 200 | −0.4 | 0.13 |
| in-memory | 2,000 | −0.9 | 1.33 |
| in-memory | 8,000 | −6.8 | 5.03 |
| sqlite | 200 | −0.1 | 0.14 |
| sqlite | 2,000 | +3.5 | 1.41 |
| sqlite | 8,000 | +1.4 | 4.95 |

The difference between the two arms is disk, and it is the whole of `close` and most of `publish`.

## Takeaways

- **The reservation stands, on evidence.** Doubling the loop's hashing still changes nothing measurable, and hashing's absolute cost has not moved across a 26x speedup of everything around it.
- **Hashing's real home is the load**, at one hash per stored op, and its cost grows with history length rather than with document size.
  5 ms on 8,000 ops; the thing that bounds it is pruning.
- **A share is two numbers.** Hashing's share of the loop grew from 0.2% to 7% while its cost stayed put, purely because the loop shrank.
  That is the reading error this table would invite without the sentence above it.
- **The mechanism this write-up first named for materialization's cost was wrong**, and the curve could not have told anyone so.
  The curve fits more than one story; the code fits one.
- **A benchmark that grows its own input measures the growth.** The control here was wrong in exactly that way before it was fixed, and it read as a real signal.
