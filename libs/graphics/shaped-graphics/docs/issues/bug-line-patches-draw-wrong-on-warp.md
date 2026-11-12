---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics/tests/raster/mixed-route-test.cc
  - file: libs/graphics/shaped-graphics/tests/shaders/sgl_mixed/primitive_stages.sgl
---

# Line patches draw wrong on WARP, even with every stage from DXC

The "line patch" case of "sg - geometry and tessellation stages of SGL's binary link against DXC's in every pairing and draw alike" fails on WARP and passes on hardware dx12 and vulkan.
The test now skips that case on software adapters, so nothing covers line patches on WARP.

## Measured

- Local WARP, `--thorough`: the reference, with every stage from DXC, draws a zigzag where hardware draws horizontal rows.
  So WARP's picture is wrong before any SGL binary is involved.
- Local WARP: an evaluation stage of SGL's binary (masks 8 to 15) draws a block filling rows 3 to 7 instead.
  Masks 1 to 7, with DXC's evaluation stage, match the reference.
- CI's WARP fails earlier, at mask 1, so the two WARPs do not agree with each other either.
- The triangle and quad patch cases pass on WARP in every pairing.

## Hypothesis

WARP mishandles the isoline domain.
This is not proven: there is no reproduction free of our code, so it does not belong in `docs/bugs-external` yet, and SGL's isoline domain stage is not cleared on WARP.
