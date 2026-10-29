---
created: 2026-10-29
refs:
  - file: libs/base/nexus/src/nexus/async-test.hh
  - file: libs/base/nexus/src/nexus/bench/run_async.hh
---

# nexus: ASYNC_BENCHMARK on a scheduler of its own, and counters for awaited work

An `ASYNC_BENCHMARK` runs on the pool nexus installs, and `no_scheduler` is refused on every async test.
So a benchmark that wants to measure async work on ONE thread, to compare against itself on every core or to read a number unpolluted by fan-out, cannot be async.
It falls back to a sync `BENCHMARK(..., no_scheduler)` that installs a single-threaded scheduler and blocks with `async_blocking_get_on`:
the very pattern async tests exist to replace.

`nx::bench::run_async` also takes no hardware-counter pass, so awaited work has wall time only.
On a loaded machine, instructions retired is the number that compares across runs, and async work is exactly where the load varies.

Wanted:
- a way for an async benchmark to run its body, or one measured graph, on a scheduler it names: a single-threaded one above all, or a pool of a chosen size;
- hardware counters around an awaited iteration, at least instructions retired, summed over the threads that ran it or reported for the awaiting thread with that caveat stated;
- both usable from `run_async`, so the same body measures on one thread and on every core side by side.
