---
created: 2026-11-11
parent: epic-sgl-every-stage-everywhere
blocked-by: [feat-sg-work-graphs]
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/gpu-driven-work.md
closed: 2026-11-11
resolution: wontfix
---

# Work graphs: the language model, native on dx12

Nodes that take typed records and emit records to other nodes, scheduled on the GPU.
D3D12 has them in SM 6.8 and 6.9, with broadcasting, coalescing and thread launch, bounded recursion, and SM 6.9's mesh nodes.
Vulkan's counterpart is `VK_AMDX_shader_enqueue`, provisional and AMD-only.

**Microsoft has reportedly frozen work graphs: no support in SM 6.10**, with work lists as the direction ([feat-sgl-work-lists](../../../feat-sgl-work-lists.md)).
So the language model must not be dx12's alone: it has to serve the polyfill and work lists as well as it serves native graphs.

To decide first, in [gpu-driven-work.md](../../../../spec/incubator/gpu-driven-work.md):

- A node as an entry point with a record input and bounded, typed output streams, checked as a mesh payload is.
- Whether the model is the graph or the list, and which is derived from the other.
- Whether native dx12 is worth it beyond being the reference the polyfill is measured against.
- Whether `VK_AMDX_shader_enqueue` is a target at all.

Before designing, read the primary sources: the D3D12 work graphs spec, and Microsoft's statement on SM 6.10.

## Decided (2026-11-11): not added

D3D12 has deprecated work graphs, with no support in SM 6.10, so SGL does not add them, natively or polyfilled.
GPU-driven work goes through work lists instead: [feat-sgl-work-lists](../../../feat-sgl-work-lists.md).
The comparison matrix says so below its D table.
