---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/stage-polyfills.md
  - issue: feat-sgl-mesh-and-task-stages
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-11-11T11:15Z
---

# The mesh and task stages on webgpu, polyfilled

dx12, vulkan and metal draw SGL's mesh and task stages; webgpu has neither and refuses them.

The polyfill is a compute pass that runs the task stage and then each mesh workgroup it launches.
The mesh workgroups write vertices and indices to a buffer, and one GPU-written `draw_indexed_indirect` draws them.
The payload is a buffer slot per launched workgroup, `set_counts` an atomic reservation, and per-triangle values a buffer the pixel stage reads by primitive index.

What is not known yet:

- Whether webgpu's per-primitive index reaches a pixel stage without the feature `primitive_index`, or the polyfill must duplicate vertices instead.
- How large the vertex buffer is when a task stage launches a GPU-chosen number of mesh workgroups: the dispatch's bound, or a host budget.

Measured against vulkan's native mesh stage on the same meshlets.
