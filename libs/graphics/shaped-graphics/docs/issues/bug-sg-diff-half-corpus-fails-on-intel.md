---
created: 2026-11-14
refs:
  - file: libs/graphics/shaped-graphics/tests/differential/corpus/half_math.sgl
  - file: docs/bugs-external/intel-float-half-round-trip-folds-away/readme.md
  - test: "sg differential - every corpus entry point computes at a precise floor what SGL's interpreter says"
  - test: "sg differential - every corpus entry point computes alike on every backend and route of this machine"
  - test: "sg differential - every corpus entry point computes what its committed reference says"
  - test: "sg differential - every corpus entry point computes on the GPU what SGL's interpreter says"
---

# The half corpus fails on Intel's Arc: dx12 elides a float's round trip through a half

On the Arc B580, driver 32.0.101.9150, all three differential tests failed on the half corpus that `8f626fed6` and `4fde5ca31` added.
`half_math.sgl`'s `known intel dx12` lines log that file's divergence now, at its own level and at the strict-floor twin's.

## Measured

- **`half_math.sgl` on dx12, DXC and native routes alike:** 1023 of 1024 elements differ from the interpreter and from `half_math.ref`.
  `(x as half) as float` comes back as `0x3efe7280` where a half holds `0x3efe8000`: the value never lost a float's precision.
- **Confirmed the driver's, standalone:** [intel-float-half-round-trip-folds-away](../../../../../docs/bugs-external/intel-float-half-round-trip-folds-away/readme.md).
  On D3D12 the cast pair comes back unrounded for all 256 inputs, under `-Gis` too, from DXIL on which WARP rounds every one.
  `f32tof16` and `f16tof32` do round there, toward zero as WARP's casts do.
  Intel's vulkan driver folds the other spelling, `PackHalf2x16` and `UnpackHalf2x16`, which SGL never writes.
- **The same module on vulkan** agrees with the interpreter bit for bit; it fails only the cross-backend comparison against dx12.
- **`half_arithmetic.sgl` no longer fails here.** Its 12 elements were `a * b - a` with `b` near 1, the product kept at a float's precision, as `.fast` allows.
  The interpreter's ranges hold them now, `feat-sgl-interpreter-bounds-float-departures`.

## Not known

- A workaround: the intrinsics round on Intel's D3D12, but toward zero, where EVAL-96 says to nearest.
- Whether D3D specifies toward zero for a float's conversion to a half: WARP does it through the casts and the intrinsics alike.
  If it does, `bug-sgl-half-conversion-rounds-toward-zero-on-amd` describes D3D's rule rather than AMD's bug.
- The report to Intel, which the standalone repro is ready for.
