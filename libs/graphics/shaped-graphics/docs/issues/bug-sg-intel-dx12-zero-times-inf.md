---
created: 2026-11-05
parent: epic-sgl-float-guarantee-ladder
refs:
  - file: libs/graphics/shaped-graphics/tests/compute/compute-float-specials-test.cc
  - test: "sg - an SGL float keeps NaNs, infinities, signed zeros and the order of a sum on the GPU"
---

# Intel's dx12 driver folds a strict 0 * inf to 0

An entry point at `@float(.specials)` compiles for dx12 with `-Gis`, and DXC then writes IEEE-strict DXIL: no `fast` flags, the NaN tests kept.
The float-specials probe's `inf * zero`, both read from a buffer, is `fmul float %6, %11` there, which IEEE makes NaN.
On an Intel Arc B580, driver 32.0.101.9150, it comes back 0, while every other check of the probe passes.
So the driver's own compiler seems to simplify `x * 0` to 0 against the strict DXIL; not yet reproduced standalone, which a docs/bugs-external entry needs.
The test skips that one check on Intel's dx12 adapters until then.
