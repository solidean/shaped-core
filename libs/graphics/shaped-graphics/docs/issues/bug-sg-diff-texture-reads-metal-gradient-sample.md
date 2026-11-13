---
created: 2026-11-13
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/texture_reads.sgl
  - test: "sg differential - every corpus entry point computes on the GPU what SGL's interpreter says"
---

# texture_reads' gradient sample is 2.4% off the interpreter on metal

On an Apple M4, relwithdebinfo, `texture_reads.sgl`'s `reads` differs from SGL's interpreter in 4 of 4096 elements, beyond relative 0.005.
Elements 8, 9 and 1176 read 0.2341, 0.1990 and so on against 0.2398, 0.2050: about 2.4 % off.
It fails with Metal's safe math as with its fast math, so it is not EMIT-262's math mode.
Hypothesis: the elements are floats, so these are the `sample` at explicit gradients (`4 * i + 2`, threads 0 and 73).
That read picks a level from the gradients and blends two by `mip_filter = .linear`, which Apple's GPU computes with less precision than the interpreter.
The entry came with 9bd59621b on 2026-11-13; it passes elsewhere, so a tolerance for that output on metal, or a probe of Metal's LOD, decides it.
