---
created: 2026-10-29
refs:
  - file: libs/base/clean-core/src/clean-core/thread/async.hh
---

# clean-core: a parallel for over an index range, on the async scheduler

There is no parallel loop: a caller fanning `body(i)` over `[0, n)` builds a vector of `async_run_on` tasks by hand and awaits them with `async_all`.
That is the most common shape of compute work, and every caller re-derives chunking, the task count and the await.

Wanted, as the obvious spelling for it:
- `cc::async_parallel_for(n, body)` returning an async that settles when every index ran, with an awaitable and a blocking form;
- chunking chosen by the scheduler from `n` and the worker count, with an optional grain size for cheap bodies;
- inline execution when `n` is below one grain or the scheduler is single-threaded, so a caller pays nothing for small ranges;
- failure of one body failing the whole, as `async_all` does.

What a consumer needs it for: a physics step whose per-element work (a few hundred nanoseconds each, thousands of elements) runs on every iteration of an inner loop.
At that granularity the per-task overhead decides whether parallelism pays at all, so the cost per chunk is the number to measure and pin with a benchmark.
