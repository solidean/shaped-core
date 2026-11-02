---
created: 2026-10-17
tags: [async, benchmark]
refs:
  - file: libs/base/clean-core/tests/benchmarks/async/async-benchmark.cc
  - symbol: nx::bench::sink
---

# The async-vs-direct tax in `async-benchmark.cc` is not believable

A 512-node chain reports 423x over the direct analog, off a direct baseline of 0.36 ns per step — under one cycle for a call plus an add.
The direct side is almost certainly folding despite the XOR-into-sink, so the column is measuring the optimizer rather than the machinery.
Every tax in that file is suspect by the same argument.
The `noinline` probes exist for the disassembly, not for the timed loop, which is the gap.

Hypothesis: the scalar `nx::bench::sink` is worth suspecting first.
It claims one value is observed and nothing else.
So a direct side that accumulates into something the compiler can carry in a register across the whole loop still folds, with the guard doing exactly what it documents.
That would make the fix a `void(isize)` body laundering the accumulator per iteration rather than anything about `noinline`.

Revisit once the benchmark architecture has settled rather than patching the numbers now.
