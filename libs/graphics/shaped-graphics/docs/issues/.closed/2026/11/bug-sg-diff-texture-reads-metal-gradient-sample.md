---
created: 2026-11-13
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/texture_reads.sgl
  - test: "sg differential - every corpus entry point computes on the GPU what SGL's interpreter says"
closed: 2026-11-17
resolution: fixed
---

# texture_reads' gradient sample is 2.4% off the interpreter on metal

On an Apple M4, relwithdebinfo, `texture_reads.sgl`'s `reads` differs from SGL's interpreter in 4 of 4096 elements, beyond relative 0.005.
Elements 8, 9 and 1176 read 0.2341, 0.1990 and so on against 0.2398, 0.2050: about 2.4 % off.
It fails with Metal's safe math as with its fast math, so it is not EMIT-262's math mode.
Hypothesis: the elements are floats, so these are the `sample` at explicit gradients (`4 * i + 2`, threads 0 and 73).
That read picks a level from the gradients and blends two by `mip_filter = .linear`, which Apple's GPU computes with less precision than the interpreter.
The entry came with 9bd59621b on 2026-11-13; it passes elsewhere, so a tolerance for that output on metal, or a probe of Metal's LOD, decides it.

## Resolved

2026-11-17: a probe on the M4 swept the level of detail over the first level in 64ths, through gradients and as an explicit level, on a texture whose mip m holds m.
The explicit level read exact; the level from gradients read in 256th steps, up to two 256ths low, so Metal computes its log2 less exactly, not its blend.
That moves a blend of two hashed texels by up to 0.008, which the four elements' 0.006 fits.
`sg - a level of detail from gradients is near their log2, and an explicit level is exact` pins both on every backend, the first within a 32nd.
The gradient samples moved to their own corpus entry, `texture_gradients`, held to relative 0.01; `texture_reads` keeps its exact reads, a second load in the sample's place.
Both references were rewritten on metal, so dx12 and vulkan meet them the next time they run.
