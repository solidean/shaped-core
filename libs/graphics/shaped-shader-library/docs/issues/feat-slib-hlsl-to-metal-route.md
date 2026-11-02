---
created: 2026-10-17
tags: [metal]
refs:
  - file: libs/graphics/shaped-shader-compiler-msl
  - file: examples/graphics/rotating-cube
---

# A shader authored in HLSL has no route to metal

`shaped-shader-compiler-msl` compiles MSL to a metallib, or to MSL source where Apple's separately installed Metal toolchain is absent.
slib's `create_metal_compiler` is the edge, and an SGL package reaches metal through it.
So a shader authored in SGL compiles for metal like any other target.

**What remains is HLSL.**
DXC publishes no macOS binary, and SPIRV-Cross is unvendored, so nothing turns HLSL into MSL on a macOS host.
`sv` is not one of the casualties any more: its path tracer and raster passes are SGL, and what is unverified is its ray-tracing pipeline on metal.

**`rotating-cube` has no metal arm**, and it is the one example that still cannot get one.
Its source is HLSL, so the cube reaches metal through `sgl-cube` instead, from a package rather than an embedded blob.
`metal-cube` was the stand-in for that and is gone, along with its copy of the geometry, the camera maths, the hand-written vertex layout and the checked-in metallib.

The hand-written metal tier-2 fixtures in shaped-graphics wait on the same route: [debt-sg-metal-hand-written-fixtures](../../../shaped-graphics/docs/issues/debt-sg-metal-hand-written-fixtures.md).
