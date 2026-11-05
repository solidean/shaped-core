---
created: 2026-11-05
refs:
  - file: libs/base/clean-core/tests/record/record-hot-functions-test.cc:292-314
  - test: "record/hot - skip_idle leaves out a parked thread's samples, and they count nowhere else"
---

# record/hot skip_idle test misses the parked thread under a loaded suite

On Windows the test requires that the sampler caught the parked thread in its wait at least once (`busy.idle_samples > 0`).
It already retries up to 10 times for a loaded machine, and on 2026-11-05 all 11 runs missed during a full `dev.py check test` with every binary in parallel.
Alone it passed 20 of 20 under `--repeat 20`, so it is load, not a regression.
A fix waits for the condition instead of a run count: keep the thread parked until a sample of it in its wait has been taken, with a cap that fails loudly.
