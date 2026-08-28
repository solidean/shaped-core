# async benchmarks (`cc::async`, `cc::async_thread_pool`)

Four benchmarks over [systems/async](../systems/async.md), answering two questions in order.
**What does one node cost on one thread**, and **does a graph scale across a pool**.
The single-thread number comes first on purpose: if driving a node is expensive, no amount of work-stealing quality can rescue it.

| benchmark | source | answers |
|---|---|---|
| single-thread drive | [async-benchmark.cc](../../tests/benchmarks/async/async-benchmark.cc) | the async tax — per-node create → drive → destroy against a hand-written analog |
| born-ready decomposition | [born-ready-benchmark.cc](../../tests/benchmarks/async/born-ready-benchmark.cc) | where a born-ready node's ~40 ns goes, as a cumulative ladder |
| work-stealing pool | [pool-benchmark.cc](../../tests/benchmarks/async/pool-benchmark.cc) | scaling across five fork-join shapes, and the per-node scheduling cost |
| grain & fork floor | [grain-benchmark.cc](../../tests/benchmarks/async/grain-benchmark.cc) | at what leaf size fork-join overhead stops dominating, and what the second thread costs |

The first three are `PGO_BENCHMARK`s, recording representative points via `nx::pgo` for the perf gate.
The single-thread and pool ones each also have a manual full-sweep twin that prints the whole table; the born-ready ladder has none, since its rows are cumulative and it always runs all of them.
The fourth is manual-only: two of its three entry points emit CSV for the plot scripts beside it, and the third prints a round-trip table.
None runs in the normal test sweep; every one is reachable by exact name.

## Method

All four share [bench_util.hh](../../tests/benchmarks/bench_util.hh): an adaptive timer that repeats a pass until ~50 ms have elapsed.
That is wrapped in a **median of 5** independent measurements, each with its own warmup pass.
Results are XOR-folded into `bench::sink` so nothing is dead-code-eliminated.

Four constraints hold across the files, and breaking any of them corrupts the numbers silently rather than loudly.

**The compiler must not fold the baseline away.**
`CC_DONT_INLINE` alone is not enough: clang infers a noinline helper is pure and propagates its return value, so a chain of `x+1` steps still collapses to `x+N`, or to a compile-time constant.
Both files therefore route their baselines through an empty `volatile` inline-asm barrier (`opaque`), which makes its argument un-analyzable *and* gives the containing function side effects.
The side effects are the load-bearing half for the mutation-free baselines.
The reduction and spawn-tree analogs are pure functions of unchanging inputs, and measured 0.00 ns until the barrier was added.
The point is a baseline that is a genuine hand-written call-based analog, **not** maximally-optimized straight-line code: that is what makes the tax an honest per-node comparison.
The single-thread cases add a second guard: every graph's leaf value is seeded from the runtime loop index.
Each graph therefore computes different, data-dependent output, and none of it can be hoisted to a constant.

**Leaf work must stay compute-bound.**
The pool and grain benchmarks mix each element with lowbias32 (Chris Wellons' hash-prospector search), and the one property that matters is that it is **not affine** over Z₂³².
An affine mix composes — a chain of `x = x*a + b` rounds collapses into a single multiply-add — which turns the parallel-for and reduction leaves into memory-bandwidth streamers.
Those measure the machine rather than the scheduler, and cap out around 8 workers.
So a case that stops scaling past ~8 workers is a codegen question before it is a scheduler question.

**Every fork-join frame must fit the node's inline slot, which on a one-line node is 24 B.**
A closure that does not fit falls back to a heap-boxed `cc::unique_function` ([node layout](../systems/async.md#node-layout-size--locking)).
That is an allocation in every task, and it makes the whole thing a benchmark of malloc.
A two-child frame captures two `shared_async` (8+8) outright, so a range has to be one word: these carry `{i32 offset, i32 count}` against a namespace-scope base pointer rather than a 16 B `cc::span`.
The `spawn` helpers ask `cc::async<i64>::frame_fits_inline<F>` rather than restating the budget.
An earlier hand-copied `<= 32` survived the slot shrinking to 24, and would have let all six frames spill silently.
That is why grain sizes are namespace-scope constants rather than captures, and why the spawn helpers `static_assert` on it: adding one capture anywhere changes what is measured.

**The pool is constructed outside the timed pass.**
Spawning threads is ~100 µs, and inside the adaptive loop it would dominate every short pass.
`pool-benchmark.cc` builds one per row; the grain sweep builds one for the whole sweep, and the fork-floor and round-trip probes one per worker count, since the worker count is what they vary.

## Reading a pool table

Two things about the numbers are not visible in the table itself.

**A "*w* worker" row runs *w*+1 threads.**
`blocking_get` makes the calling thread borrow a pool slot and participate for the duration — see [Driving](../systems/async.md#driving-the-scheduler-seam).
So the sweep tops out at hardware concurrency **minus one**, and the `vs 1w` column is anchored on a 2-thread config rather than a serial one.
That is not a rounding detail.
At 32 workers on 32 hardware threads, the extra thread cost the reduction case 2× (0.04 vs 0.02 ns/elem) in pure scheduler thrash — which reads as a scheduler defect and is not one.

**The serial baseline is re-measured on every row, adjacent to that row's pool run.**
Measuring it once up front made the speedup ratio a lie.
Sustained all-core load downclocks a laptop, so a baseline taken seconds earlier on a cool chip was being divided by a pool time taken on a hot one.
An early version of `pool-benchmark.cc` read 0.65 ns/elem serial in the full sweep and 0.26 in the guide run — same code, same machine, 2.5× apart, purely from what had run before it.
Adjacent measurement makes both halves of the ratio see the same machine.
The serial column then doubles as a **contamination canary** — nothing in the file touches it.
A serial column that drifts down the table means the run is dirty, and its cross-row numbers are not comparable.

So the three columns are not equally trustworthy:

| column | what it is | trust |
|---|---|---|
| `serial` | the adjacent baseline | the canary — flat is clean, drifting invalidates the case's cross-row numbers |
| `vs serial` | serial ns / pool ns | an adjacent pair, so it survives throttling |
| `vs 1w` | the scheduler's own scaling | 1w and Pw are rows apart in time — trust it only where the canary above is flat |

Judge near-linearity against the count of **performance** cores, not the thread count: the curve bends at the E-core and SMT boundaries by design.
The spawn tree's `vs serial` is expected to be ~0 and is not a defect — its serial analog is a bare recursive call against a whole scheduled node, so read its ns/node and `vs 1w` instead.

## Results

Measured on an i9-12900H (6P+8E, 20 threads), `relwithdebinfo-clang`.
A snapshot of where the time goes, not a contract — re-measure before quoting one as fact.

### Scaling (pool at P vs pool at 1 worker)

Independent of leaf cost, and to be judged against the **6 P-cores** rather than the 20 threads.

| case | scaling | note |
|---|---|---|
| reduction | **6.08×** | at/near the machine's ceiling |
| parallel quicksort | **5.42×** | irregular subproblems — the steal-quality case |
| spawn tree (131071 nodes, trivial leaves) | **4.97×** | 60.7 → 12.2 ns/node across the worker sweep |
| nested parallel-for | 4.73× | a parallel-for whose leaf is itself a parallel-for — deque depth + nested spawn |
| parallel-for transform | 4.51× | recursive bisection, perfectly balanced |

The spawn tree is the pure-overhead metric: its leaves do nothing, so its ns/node **is** the pool's per-node scheduling cost, and it is correspondingly the most sensitive case to idle-worker policy.

### The single-node ladder

A born-ready async (`make_async_from_value` → `try_value` → destroy) is exactly one node-allocator alloc + free plus the node's own control/payload/state bookkeeping.
The full cycle is ~40 ns and the 64 B slab alloc+free pair is ~2.9 ns, so **~37 ns is non-allocator overhead** — which is what the decomposition ladder splits apart, one component per row:

| row | isolates |
|---|---|
| raw node alloc+free | the allocator floor for `async<int>`'s own node class — also the canary, since nothing else touches it |
| + `make_async_manual` | `make_shared` control init (intrusive refcount) + node ctor/dtor |
| + `make_async_from_value` | `push_value`: build the value slot, state → `ready_value`, teardown |
| + `try_value` | the zero-copy value read — this row is the full born-ready total |
| cold `make_async_lazy` | measured against the empty node instead: a lazy frame/closure build + destroy, never driven |

Deltas are cumulative against the row above, so the whole ladder always runs; `record` only selects which rows are filed as gate metrics.
The manual and cold-lazy rows drop an **unresolved** node on purpose, and that must stay safe — frame torn down, no leak, no assert.
A failure there is a bug in async teardown rather than in the benchmark.

Instruction counts for the same two points, from the pinned disassembly probes:

| case | cost |
|---|---|
| `make_async_manual<int>` created and dropped — no frame, no scheduler | **128 instructions, zero locked RMW** |
| driven `make_async_lazy<i64>` leaf — the full create → drive → destroy cycle | **509 instructions, 8 atomics** |

For 64 B of storage plus an alloc *and* a dealloc, that is not alarming.
[Cost](../systems/async.md#cost) reads those counts as a design constraint — which of the eight atomics are inherent, and which are the queue round-trip.

### Grain, and the fork floor

The pool benchmark pins one grain per case — 8192 for parallel-for and reduction, 1024, 4096 and 65536 for the others — and sweeps worker count.
So by construction it never shows the region those grains were chosen to avoid.
At 8192 elements per leaf, per-node scheduling cost is ~1/8192 of the leaf and simply invisible.
The grain sweep moves the other axis — grain and *n*, both by powers of two, on one default-sized pool (hardware threads − 1) — so that cost **is** the measurement.

Read it as ns per **input** element.
For leaf work costing X ns/elem and per-node scheduling cost C, a grain-*g* bisection lands near X + C/*g*.
Each grain line is therefore flat where *n* is large and the overhead is amortized, and rises toward small *n* where the `blocking_get` round-trip is spread over too few elements.
The vertical spread between lines at a fixed *n* is C/*g* — the thing the sweep exists to show.
Only grain ≤ *n* is measured, since a grain above *n* never splits, so each line starts at *x* == grain.

At the small-*n* end every line runs into a **floor**, and that floor — not per-node cost — is what makes small graphs expensive.
A graph that forks even once lands on a plateau that holds out to a few thousand elements, well above what an un-split single node costs.
The suspect is the pool waking a worker to take a published sibling, and `run_fork_floor` sweeps the two axes that tell the candidates apart.
Pool size 1..8: a **fixed** handoff cost stays flat there, whereas contention grows with the number of idle workers scanning the injection mutex.
And *n* 1..32: how the floor amortizes once the graph has real work to do.
Read the `w=1` line first — one worker plus the participating caller is the minimum fork, so it is the floor's floor, and anything above it that grows with *w* is the pool getting in its own way.

> **The floor's absolute size is unmeasured, deliberately.**
> Two comments in the benchmark disagreed about it by roughly 70× for the same configuration, so both figures were deleted rather than one being picked.
> What is not in dispute: an inline node is ~50–100 ns, and the floor is microseconds — three to four orders of magnitude of pure handoff.
> Treat the *shape*, flat-vs-fanning across worker count, as the result here, and re-measure on one run before quoting a number.

## Running them

The PGO benchmarks and their full-sweep twins are excluded from normal sweeps; an exact (non-wildcard) name runs a test regardless of bucket.

```bash
# guide points, recorded for the perf gate
uv run dev.py test "bench-async (single-thread drive)" --preset release-clang
uv run dev.py test "bench-async born-ready decomposition" --preset release-clang
uv run dev.py test "bench-async-pool (work-stealing)" --preset release-clang

# the human-facing tables
uv run dev.py --mirror-test-output test "bench-async (single-thread drive - full sweep)"
uv run dev.py --mirror-test-output test "bench-async-pool (worker sweep)"
```

The grain sweeps run for minutes, so `--timeout 0` is not optional — `dev.py` kills a test binary at 60 s by default and would cut the table off mid-run.
Either drive them directly, or let the plot scripts do it and chart the result:

```bash
uv run dev.py benchmark "bench-async-grain - grain x size sweep" --timeout 0
uv run dev.py benchmark "bench-async-grain - fork floor thread sweep" --timeout 0
uv run dev.py benchmark "bench-async-grain - drive round-trip"

uv run libs/base/clean-core/tests/benchmarks/async/grain-plot.py
uv run libs/base/clean-core/tests/benchmarks/async/fork-floor-plot.py
```

Each plot script saves its raw stdout next to the PNGs, so `--input raw.txt` re-plots a capture for free.

Three disassembly probes are pinned symbols kept alive by their PGO benchmarks — two on the born-ready ladder, one on the driven leaf:

```bash
uv run dev.py assembly show make_async_lazy_probe          # cold lazy node: must show exactly ONE alloc path
uv run dev.py assembly trace --target clean-core-test --symbol make_async_manual_probe --stats \
  -- "bench-async born-ready decomposition"
uv run dev.py assembly trace --target clean-core-test --symbol single_lazy_probe --skip 2 --stats \
  -- "bench-async (single-thread drive)"
```

`--skip 2` on the driven leaf is not optional: the first enqueue grows the scheduler's queue vector from zero capacity, a real allocator call the reused-scheduler steady state never pays.

Use a `release` preset and an otherwise-idle machine, and discard the first post-build run — see [docs/guides/perf-results.md](../../../../../docs/guides/perf-results.md).
