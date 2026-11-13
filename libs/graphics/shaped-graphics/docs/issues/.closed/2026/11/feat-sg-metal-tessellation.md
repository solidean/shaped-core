---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
refs:
  - file: libs/graphics/shaped-graphics/docs/concepts/raster-pipeline.md
closed: 2026-11-12
resolution: obsolete
---

# Tessellation on metal

sg's tessellation pair runs on dx12 and vulkan; metal has tessellation and sg does not drive it.
Metal's model: a compute dispatch writes per-patch factors, then a draw of patches whose vertex function is a post-tessellation one.

So a tessellated raster pipeline on metal is a compute pipeline, a factors buffer, and a render pipeline with a tessellation descriptor.
A draw records the dispatch ahead of itself.
SGL's side, which writes both shaders, is [feat-sgl-metal-tessellation](../../../../../../shaped-graphics-language/docs/issues/.closed/2026/11/feat-sgl-metal-tessellation.md).

## Closed

Metal 4 has no tessellation: `MTL4RenderCommandEncoder` has no `drawPatches` and `MTL4RenderPipelineDescriptor` no tessellation state, and sg's metal backend is Metal 4 throughout.
Metal takes the compute tessellator instead, [feat-sgl-tessellation-polyfill](../../../../../../shaped-graphics-language/docs/issues/feat-sgl-tessellation-polyfill.md).
