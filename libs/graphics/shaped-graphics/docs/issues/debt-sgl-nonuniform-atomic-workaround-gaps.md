---
created: 2026-11-23
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/core.sgl
  - file: docs/bugs-external/nonuniform-indexed-atomics-count-other-lanes/readme.md
---

# CHK-553's per-lane form is still wrong now and then on Intel, and a pixel stage keeps its native atomic

CHK-553 writes an atomic at a `nonuniform` binding-array index around Intel's and AMD's drivers, but only its leader form is right on both.
The leader form needs a compute stage with subgroups, a 32-bit integer, and on vulkan `quad_control`; for `min`, `max` and the bit operations, also a value before that nothing reads.

- **Everything else takes a compare-exchange per lane**, which is right on AMD and wrong on Intel in about 1 run of 20 (measured: 9 of 200 in the reproduction).
  A failing run adds once too often, which fits Intel storing a compare-exchange through a non-uniform handle while reporting it failed; that is a hypothesis.
  That covers every non-compute stage, 64-bit atomics, and `min` to `bit_xor` with their value read.
- **A pixel stage keeps the native atomic**, wrong on both, since SGL has no helper-lane query to keep a helper out of a loop.
- **Measured not to work**, so not to retry: one lane at a time, natively or by compare-exchange, with or without rounds per index; a compiler turns such a loop back into the per-lane atomic.

What would close most of it:
- **A fixed array of up to 16 elements** could branch to each element by its literal index, which is right on both drivers in every stage, as SGL's AMD length workaround already does.
- **64-bit subgroup reductions** in SGL would let 64-bit atomics take the leader form.
- **An exclusive scan of `min`, `max` and the bit operations** would give each lane of a round its own value before, so those take the leader form too.

`sg - an atomic at a nonuniform index into an SGL binding array is right in each form CHK-553 writes` skips the per-lane form on Intel for this reason.

Once, in 70 runs of the binding-array tests on the AMD iGPU, dx12 hung the device (`DXGI_ERROR_DEVICE_HUNG`).
It was not seen again in 80 more runs, and which shader hung is unknown; a loop of the workaround that a lane never leaves would look like this.
