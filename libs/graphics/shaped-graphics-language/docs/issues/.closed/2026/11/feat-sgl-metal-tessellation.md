---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
blocked-by: [feat-sg-metal-tessellation]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/stage-polyfills.md
closed: 2026-11-12
resolution: obsolete
---

# Tessellation on metal, natively

SGL's tessellation stages draw on dx12 and vulkan and are refused on metal, which has tessellation of its own.
Metal's model is a compute kernel that writes per-patch factors to a buffer, the fixed-function tessellator, and a post-tessellation vertex function that evaluates.

So the control stage lowers to that kernel, and the evaluation stage to the post-tessellation vertex function, in MSL.
What is not known yet:

- Whether every control-stage output SGL allows has a place in Metal's factor and per-patch buffers, or some need a buffer of their own.
- Which partitioning modes and output topologies match one to one, measured against dx12's draw of the same patches.
- Whether `tessellation_shader` stays the feature, granted on metal, or metal grants it as a different one.

## Closed

Metal 4 has no tessellation: `MTL4RenderCommandEncoder` has no `drawPatches` and `MTL4RenderPipelineDescriptor` no tessellation state, and sg's metal backend is Metal 4 throughout.
Metal takes the compute tessellator instead, [feat-sgl-tessellation-polyfill](../../../feat-sgl-tessellation-polyfill.md).
