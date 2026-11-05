---
created: 2026-11-05
parent: epic-sgl-float-guarantee-ladder
refs:
  - file: libs/graphics/shaped-graphics/tests/compute/compute-float-specials-test.cc
  - file: docs/bugs-external/intel-d3d12-strict-reciprocal-product-folds-to-one/readme.md
  - test: "sg - an SGL float keeps NaNs, infinities, signed zeros and the order of a sum on the GPU"
---

# Intel's dx12 driver folds a strict (1 / x) * x to 1 where x is zero

The float-specials probe computes `inf` as `one / zero`, then `inf * zero`, which IEEE makes a NaN.
On an Intel Arc B580, driver 32.0.101.9150, it comes back 1.0 even from `-Gis` DXIL, which marks no operation `fast`.
`0 * inf` with the infinity read from a buffer stays a NaN: the driver cancels the reciprocal against the multiply.
[intel-d3d12-strict-reciprocal-product-folds-to-one](../../../../../docs/bugs-external/intel-d3d12-strict-reciprocal-product-folds-to-one/readme.md) reproduces it standalone, against WARP.
Intel's vulkan driver does the same with strict SPIR-V, seen in the same test once it ran on vulkan.
The test skips that one check on Intel's hardware adapters until a driver passes `run.py`.
