# sort benchmark: `cc::sort` vs `std::sort`

What the swap-only formulation costs and what it buys, measured against `std::sort` across input shapes, element types and sizes.
The benchmark is [tests/benchmarks/sort-benchmark.cc](../../tests/benchmarks/sort-benchmark.cc); run it with

```bash
uv run dev.py benchmark "bench-sort" --timeout 0
```

Numbers below: Windows, clang-cl, `release-clang`, single run, millions of elements sorted per second.
Each point is the median of 5 adaptive timings with the cost of restoring the unsorted input measured out separately and subtracted.

**Those numbers predate the nexus benchmark harness**, which medians hundreds of samples and brackets each with a confidence interval.
They are re-measured when this write-up is next revised.

`ratio` is `cc / std`, so above 1.0 is `cc::sort` ahead.

## The two findings

**1. `cc::sort` loses roughly 2× on tiny random inputs.**
At n = 16 — inside the insertion-sort threshold — random input runs at 0.42–0.67× of `std::sort`, consistently across all four element types.
This is the predicted cost of the design: shifting one position by swapping writes three times where a move-based insertion sort writes once.
It is the measurement that would justify a move-based small-sort fallback, and the only place the swap-only rule is clearly expensive.

Note the same n = 16 row is *faster* than `std::sort` on `sorted`, `mostly_sorted`, `sawtooth` and `push_middle` (1.06–4.49×), so this is about random input at small n, not about small n.

**2. It wins large, and wins enormously on patterns.**
At n = 1 000 000 random it is 2.5× ahead for `i32` and `u64`.
On the patterned inputs pdqsort exists to defeat — `sorted`, `reverse`, `push_front`, `push_middle` — it runs 2.9–9.9× ahead at a million elements.

The one pattern where `std::sort` leads at scale is `all_equal` (0.39–0.72×), and `two_values` / `few_values` are mixed.
Both sides are in the 0.1–3.4 G elem/s range there, i.e. already near-linear; this is not a degeneration, just a less tuned constant.

## i32

| pattern | n=16 | n=1000 | n=1M |
|---|---|---|---|
| random        | 0.46 | 1.30 | **2.48** |
| sorted        | 1.34 | 3.68 | **9.40** |
| reverse       | 0.78 | 2.95 | **6.25** |
| mostly_sorted | 1.33 | 1.12 | 1.21 |
| all_equal     | 1.34 | 0.39 | 0.39 |
| two_values    | 0.71 | 0.61 | 0.78 |
| few_values    | 0.62 | 0.95 | 1.80 |
| organ_pipe    | 0.56 | 1.21 | 0.95 |
| sawtooth      | 1.29 | 1.17 | 1.61 |
| push_front    | 0.87 | 1.37 | **3.73** |
| push_middle   | 1.06 | 2.54 | **5.10** |

Absolute, for scale: random 1M is 51.1 M/s (cc) against 20.6 M/s (std); sorted 1M is 1432 M/s against 152 M/s.

## The other element types

Same shape, so only the headline rows:

| type | random 1M | sorted 1M | reverse 1M | push_middle 1M | random 16 |
|---|---|---|---|---|---|
| `i32`        | 2.48 | 9.40 | 6.25 | 5.10 | 0.46 |
| `u64`        | 2.53 | 9.85 | 7.89 | 5.49 | 0.42 |
| `wide` (64 B)| 1.00 | 5.21 | 2.89 | 1.96 | 0.44 |
| `cc::string` | 1.07 | 6.39 | 5.68 | 5.27 | 0.67 |

`wide` and `cc::string` take the non-branchless partition and hold the pivot by reference, so the block-partition win disappears and random input lands at parity.
The pattern wins survive, because they come from the pattern-defeating machinery rather than from branchless classification.

## Parallel: `cc::sort_async`

A separate benchmark in the same file, run with

```bash
uv run dev.py benchmark "bench-sort - sort_async" --timeout 0
```

Same machine, `i32`, against serial `cc::sort` re-measured on every row so a machine that clocks down mid-sweep moves both columns together.
`workers` is the pool's worker count; the calling thread participates on top of it, so `1` already means two threads doing work.

Speedup over serial `cc::sort`, at the shipped cutoff:

| pattern | n | 1 | 2 | 4 | 31 |
|---|---|---|---|---|---|
| random     | 1M | 1.94 | 2.20 | 3.03 | **5.02** |
| random     | 4M | 1.42 | 2.28 | 3.51 | **6.92** |
| sorted     | 1M | 0.96 | 0.97 | 0.97 | 0.99 |
| sorted     | 4M | 0.98 | 0.99 | 0.95 | 0.86 |
| few_values | 1M | 0.90 | 0.92 | 0.92 | 0.90 |
| few_values | 4M | 1.14 | 1.37 | 1.28 | 1.39 |

Random 4M is 45.8 M/s serial against 317 M/s on 31 workers.

**Only random input scales, and that is the design working as intended rather than a shortfall.**
`sorted` and `few_values` are the patterns the pattern-defeating machinery already finishes in near-linear time, so there is no work left to spread.
What the parallel driver adds there is scheduling, hence the 0.86–0.99.
A caller who knows their input is nearly ordered should stay on `cc::sort`.

### Why it stops at ~7×, not at ~11×

The partition is sequential, so the work is Θ(n log n) while the span is the spine of partitions, n + n/2 + n/4 + … ≈ 2n.
That bounds the speedup at roughly log₂(n)/2, which is about 11× at n = 4M — not the 3–5× a plain Amdahl reading suggests.

Measured falls short of that bound for a second reason worth naming separately.
The top few partitions are single-threaded passes over the *whole* array and are DRAM-bandwidth-bound rather than compute-bound, so they neither speed up nor overlap with anything.

That distinction is what tells a future reader whether ips4o is worth the invariant it would cost.
It parallelises exactly those top passes, and it is the one thing that could move this number.

### The cutoff sweep

Random, n = 4M, 31 workers.
This is what fixes `cc::sort_async_default_cutoff`, which would otherwise be a guess.

| cutoff | speedup |
|---|---|
| 1024   | **7.15** |
| 4096   | 6.94 |
| 16384  | 6.67 |
| 65536  | 5.99 |
| 262144 | 4.15 |

Monotone: smaller is better across the whole range measured, because a finer grain is what keeps 31 workers fed.
The shipped default is **4096**, one step off the measured optimum — it keeps a quarter of 1024's task count for about 3% less speedup, and each task costs one heap-boxed frame.

## What is not measured here

* Only Windows / clang-cl.
  The insertion-sort threshold (16) and the block-partition gate were picked to be measured, not guessed, and neither has been swept yet.
* `sort_multi` against sort-indices-then-permute is `bench-sort - sort_multi vs sort_indices then permute`, not tabulated here.
* `sort_async` on anything but `i32`, and on any pattern beyond the three above.
  A wide element type would shift the balance toward the parallel side, since it makes the serial partition more expensive without changing the scheduling cost.
* `std::sort` is the only baseline; there is no comparison against pdqsort upstream, which would separate "our formulation" from "pdqsort itself".
