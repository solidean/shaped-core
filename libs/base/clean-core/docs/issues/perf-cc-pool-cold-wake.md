---
created: 2026-10-29
refs:
  - file: libs/base/clean-core/src/clean-core/thread/async_thread_pool.cc
  - symbol: cc::async_thread_pool::wake_all
---

# clean-core: waking a sleeping pool takes ~50 us at 31 workers

A parallel phase after a millisecond of serial work waits ~50 us for a sleeping 31-worker pool to wake, on Windows, against a loop of ~20 us.
It is the same whether the helpers start as a tree of nodes or through the broadcast slot: one notify_all releases the sleepers ~1.6 us apart.
That each woken thread reacquires the one wait mutex in turn is the hypothesis, unmeasured.
Idle linger hides it only for gaps shorter than the linger, and burns the cores meanwhile.
Candidates: per-worker parking (a futex or WaitOnAddress word per worker) so a wake does not convoy on one mutex, and waking in a tree, each woken worker waking two more.
The parallel-loops section of docs/benchmarks/async-benchmark.md has the numbers, and parallel-for-benchmark.cc's second table is the measurement to rerun.
