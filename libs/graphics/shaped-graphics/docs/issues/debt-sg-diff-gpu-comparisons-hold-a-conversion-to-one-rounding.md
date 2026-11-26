---
created: 2026-11-25
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_math.sgl
  - test: "sg differential - every corpus entry point computes what its committed reference says"
  - test: "sg differential - every corpus entry point computes alike on every backend and route of this machine"
  - issue: bug-sgl-half-conversion-rounds-toward-zero-on-amd
---

# The GPU-to-GPU differential tests hold a conversion to a half to one rounding, where EVAL-96 allows two

EVAL-96 lets `x as half` round to nearest or toward zero, as the target does, since D3D rounds toward zero and Vulkan lets the device choose.
The test against the interpreter holds that: a file held `exact` passes an element equal to either end of its range.
The reference test and the test across one machine's backends hold one GPU to another, bit for bit, and know nothing of ranges.
So a GPU that rounds toward zero, AMD's, differs from a reference written on one that rounds to nearest, though both are right.

`half_math.sgl` therefore keeps `known amd dx12` and `known amd vulkan` lines, scoped `across`: they log in those two tests, and the interpreter test holds AMD as it holds every GPU.
`half_arithmetic.sgl` has them too, since its operands are conversions: 14 of the 64 referenced elements differ from the reference by more than 0.001 on AMD's iGPU.

## Fix

Hold a GPU to a reference through the interpreter's ranges rather than bit for bit: an element passes where both are ends of one range, or where it is within the file's tolerance as now.
The reference could then commit its machine's results with the ranges beside them, or the test could run the interpreter for the ranges as the interpreter test does.

## Log

**2026-11-26: done for every file not held `exact`.**
The routes, the machine's backends and the reference now pass an element where both GPU results lie within the bound of the interpreter's range.
`half_arithmetic.sgl`'s AMD lines came out with it.
`half_math.sgl` is held `exact`, so its reference is a hash per output and has no element to hold to a range; its AMD lines stay.
What is left is one of two:
- a reference of an `exact` file whose interpreter run is ranged commits its elements rather than a hash;
- or the reference test holds that file to the interpreter's two roundings alone.
