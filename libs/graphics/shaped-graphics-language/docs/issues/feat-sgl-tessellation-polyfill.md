---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/stage-polyfills.md
---

# The tessellation stages on webgpu, polyfilled

webgpu has no tessellation, and SGL refuses the stages there.
The polyfill is a compute pass: evaluate the control stage's factors, generate each patch's domain locations, evaluate the evaluation stage at each, and draw the buffer.

Metal is not this issue's: it tessellates natively, [feat-sgl-metal-tessellation](feat-sgl-metal-tessellation.md).
If that lowering puts the factors kernel in place first, the webgpu polyfill can share it, with only the tessellator itself in compute.

Open: the grid when factors vary per patch (the largest factor's, or a prefix sum first), and whether a vertex order unlike the native one is acceptable.
Measured against dx12's native tessellation on the same patches.
