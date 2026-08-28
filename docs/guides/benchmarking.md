# Benchmarking

`BENCHMARK`, `nx::bench::run`, and `dev.py benchmark`: measuring something and being able to believe the number.
Back to [guides](_index.md).

> **This is the benchmarking framework.**
> For a small, stable set of numbers tracked across builds and consumed by `dev.py pgo`, you want a PGO benchmark
> instead — [perf-results.md](perf-results.md).
> The two are different tools: this one answers "is A faster than B, and by how much", and that one answers "has this
> number drifted since last month".

## The shortest complete benchmark

```cpp
#include <nexus/bench/run.hh>
#include <nexus/test.hh>

BENCHMARK("cc::sort - i32")
{
    auto const source = random_i32s(1 << 16);
    auto work = cc::vector<i32>();

    nx::bench::run("cc::sort",
                   [&](nx::bench::iteration& it)
                   {
                       it.pause();
                       work = source;
                       it.resume();

                       cc::sort(work);
                       nx::bench::sink(work[0]);
                       it.items(source.size());
                   });
}
```

```bash
uv run dev.py benchmark "cc::sort"
```

Every default produces a defensible measurement, so a body and a name is a complete benchmark.

## Three body signatures

`nx::bench::run` detects which one you wrote.

| signature | what it buys |
|---|---|
| `void()` | the minimal form; nothing of the harness sits between iterations |
| `void(iteration&)` | `pause`/`resume`, `items`, `record` |
| `void(isize count)` | the body owns the inner loop, so there is one timing boundary per batch |

The last is the escape hatch when the `overhead_significant` warning fires.
Moving the loop inside the body puts the harness's per-iteration cost to zero by construction, which is why the
warning names it.

**The body must be repeatable.**
It is invoked many times — across samples, across hardware-counter passes, across `--repeat` — and its observable
effect has to be the same every time.
A harness that takes a callable rather than a range-`for` is exactly what buys those, and it is why the contract
exists.

## Keeping the work alive

A benchmark measures code the compiler is free to delete.
`nexus/bench/barriers.hh` is what stops it, and the four constructs there are not interchangeable.

```cpp
auto const h = nx::bench::keep(hash(key));   // read guard; wraps an expression, costs nothing
nx::bench::sink(result);                     // the statement form
nx::bench::sink(cc::as_bytes(buffer));       // a buffer's CONTENTS; clobbers memory, so it costs more
nx::bench::compiler_barrier();               // compile-time only, over all of memory. Zero instructions.
nx::bench::evict_data_caches();              // run-time, costs milliseconds, puts the MACHINE in a cold state
```

**`keep` on a container keeps the handle, not the data.**
`keep(vec)` claims the pointer and size are read and says nothing about the bytes, so a loop that fills a vector and
keeps it can still have its stores removed.
The span overload of `sink` is the guard for contents.

**`evict_data_caches` is approximate, and what it does not do matters.**
It does not evict the TLB, the branch predictors or the instruction cache, so a benchmark calling it and reporting
"cold" is claiming something stronger than it measured.
Call it between `pause` and `resume` so its milliseconds land outside the measurement.
Its default size is a provisional constant until shaped-core has a system-information library to ask for the
last-level cache size.

## Comparing several things

Several loops in one `BENCHMARK` become rows of one table, compared against the first declared unless one asks
otherwise with `.is_baseline = true`.

```cpp
BENCHMARK("sort - ours against the standard's")
{
    nx::bench::run("cc::sort", body_a);
    nx::bench::run("std::sort", body_b);
}
```

Measuring them together is the point: two numbers from separate runs happened at different times on a machine whose
state changed in between.

**A difference whose interval spans zero prints as `~same`, not as a number.**
The comparison interval is built conservatively from the two medians' own intervals, so it can only ever be too wide —
it never claims a difference that is not there.

## Sweeps

A sweep is an outer loop with `cc::format`-built names, not new machinery:

```cpp
BENCHMARK("cc::sort - sweep")
{
    for (auto const n : {1 << 8, 1 << 12, 1 << 16, 1 << 20})
    {
        auto const source = random_i32s(n);
        nx::bench::run(cc::format("n={}", n), {.min_time_secs = 0.05}, body);
    }
}
```

**Watch the arithmetic.**
`min_time_secs` defaults to 0.5, so twenty points at the default is ten seconds of measurement plus warmup.
Lowering it per sweep is what keeps that in hand.

**Set `.no_baseline = true` on a sweep's loops.**
Otherwise every point is compared against the first, which for a size sweep is a statement about the input sizes
rather than about the code — three orders of magnitude apart it reads as a percentage in the millions.
One loop setting it drops the comparison column from the whole table, and `items/s` is what stays comparable.

**A pause is worth nothing on a body smaller than the clock.**
A pause/resume pair costs about 14 ns, so excluding a 10 ns setup from a 20 ns body measures mostly the clock — and
the harness says so.
Where a sweep reaches sizes that small, do several per iteration and let the pair amortize:

```cpp
auto const copies = cc::max(isize(1), isize(4096) / n);   // one block per iteration, whatever n is
```

The per-item rate is unchanged by construction, so the small points stay comparable with the large ones.

## Reading the report

```
host  windows x64  |  unknown  |  20 logical cores  |  build release  CC_ASSERT=off  (system info provisional)
load  clock 2.918 -> 2.918 ticks/ns (+0.0%)  |  machine 8% busy

nx::bench - the barriers
  empty loop                                 0.1140 ± 0.0002 ns   baseline
  two guards per iteration                   0.117 ± 0.002 ns     +2.6%
  compiler_barrier                           0.115 ± 0.001 ns     +0.9%
  sink over a span (a clobber, not a read)   0.2176 ± 0.0003 ns   +90.9%
```

**The value is printed to exactly the decimal place its interval reaches**, and no further.
So `0.117 ± 0.002` stops at the thousandths because that is where the uncertainty lives, while `0.1140 ± 0.0002`
earns a fourth decimal.
Under colour the `± 0.002` is drawn muted, but the text is the same.
A value whose interval reaches past its leading digit prints as `unstable`, because that is a failed measurement
rather than a wide error bar.

The host line is meant to be copied along with the number.
`CC_ASSERT` is the field that decides whether the number means anything: it is the difference between benchmarking
`cc::vector` and benchmarking its bounds checks.

One loop gets the full statistics block instead of a table row, since with nothing to compare against the width is
better spent on depth.

## The warnings

| warning | what it means |
|---|---|
| `overhead_significant` | the harness is over 2% of the per-iteration time — take the `void(isize)` form |
| `body_deleted` | per-iteration time is below the empty-loop floor, so the body was optimized away. **An error**, and it fails the benchmark |
| `did_not_converge` | sampling hit a bound without reaching the target precision |
| `paused_fraction_high` | a pause/resume pair's two clock reads are a real part of the per-iteration time. **Not** "a lot of wall time was paused" |
| `too_few_samples` | the sample count supports no interval, so the reported one is the sample range |

## The driver

```bash
uv run dev.py benchmark                      # list every benchmark in the repo
uv run dev.py benchmark "cc::sort"           # build and run everything matching
uv run dev.py benchmark "x" --json out.json  # every statistic and every sample
uv run dev.py benchmark "x" --rec out.ccrec  # a recording of the whole run, warmup included
uv run dev.py benchmark "x" --verbose-report # the full block under every table row
uv run dev.py benchmark "x" --repeat 5       # run the selection five times
uv run dev.py benchmark "x" --pin            # pin to one core, reporting whether it worked
```

**This is the one `dev.py` command that does not default to the repo's usual preset.**
`relwithdebinfo-*` compiles `CC_ASSERT` in, so it defaults to the platform's `release-*` instead.
Every existing benchmark header used to say "pass `--preset release-clang`" by hand; this is that, made the default.

A match may select several benchmarks, unlike `dev.py example` — running a family together is the normal case.
Benchmarks in one binary share a process, so they share one system summary and one `.ccrec`.

## What a benchmark is, to nexus

`BENCHMARK` is a `TEST` in the `benchmark` bucket, so it never runs in a normal sweep.
Naming one exactly still runs it, as with any other bucket.

Three things are baked in: the bucket, `exclusive_global` (two tests sharing a machine share its caches and its
memory bandwidth, so a timing taken while another runs is a timing of the pair), and `main_thread`.

**`main_thread` rules out two combinations, and both assert.**
`own_pool` — a private pool's worker is never the main thread — and an async body.
So a benchmark of thread scaling or of async code cannot use `BENCHMARK` today: declare it as a plain `TEST` with
`nx::config::benchmark` and no `main_thread`.
A macro for that case is deliberately absent until async benchmarks are actually needed.

`nx::bench::run` also works outside a `BENCHMARK` — in a manual test, or in an application — and simply hands its
result back rather than reporting it.

## Statistics, briefly

Iterations are grouped into **batches** timed as one span, and each batch's mean is one **sample**.
The batch size is calibrated, never chosen by the author: a clock pair costs about 14 ns on a typical desktop, which
cannot time a one-nanosecond body directly.

The median is the headline, with min, max, mean, a trimmed mean and the MAD alongside.
The interval on the median comes from the **order statistics** rather than a bootstrap — exact for the median of iid
samples, deterministic, and free next to the sort the statistics already pay for.
Below six samples no interval exists at all, and the report says so rather than inventing a narrow one.

**Outliers are counted and never dropped.**
A benchmark whose samples split into two modes has learned something, usually a frequency change or a cache effect,
and trimming it away silently is how a harness reports a clean number for a dirty run.

Standard deviation is deliberately absent: it needs a square root, which at this tier means either `<cmath>` (denied
repo-wide) or a typed-geometry dependency taxing every test binary.
The full sample vector travels in `--json`, in the order taken, so anyone who wants it has it.

## Hardware counters

Counters run in passes **after** the timing has converged, never inside a measured region — so an unavailable PMU
costs the timings nothing, and a present one does not change them either.
They are far less noisy than times, which is what lets them decide a comparison in fewer samples.

Where the PMU is unreachable the run says so once and carries on; `docs/guides/profiling.md` has the one-time grant
Windows needs.

## Related

- [perf-results.md](perf-results.md) — PGO benchmarks, the other thing, and the `.pgo.json` contract.
- [profiling.md](profiling.md) — hardware counters per region, and what each platform can measure.
- [nexus cheat-sheet](../../libs/base/nexus/cheat-sheet.md) — the symbols, in one page.
- [building-and-testing.md](building-and-testing.md) — the `dev.py` loop everything here sits in.
