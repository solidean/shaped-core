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
The mesh workgroups write vertices and indices to a buffer, and one indexed draw of it follows.

## Decided (2026-11-11)

**sg and SGL meet at a documented polyfill protocol.**
SGL emits what the protocol asks for, and sg draws a polyfilled pipeline through the same `draw_mesh_tasks` a native one takes, recording the compute ahead of the draw itself.
The host sees no difference beyond the feature it no longer needs.

**Each mesh workgroup writes fixed slots, never an atomic append.**
`@mesh(vertices = N, triangles = M)` bounds a workgroup's output, so workgroup `g` owns vertices `g * N` onward and triangles `g * M` onward.
Triangles past `set_counts` are written degenerate, and draw nothing.
This keeps the primitive order D3D12 and Vulkan specify for mesh shading, workgroup by workgroup and then by primitive index.
That order decides which triangle wins an equal depth and the result of a blend, which an append in completion order would lose.
A task stage keeps it with a prefix sum over the mesh workgroups each task workgroup launches.

**The scratch buffer is sg's, sized per draw, reused across draws.**

- Mesh alone: exact, the draw's workgroup count times the bytes the protocol states per workgroup; nothing to tune.
- With a task stage, the count is chosen on the GPU, so the bound is a declared maximum of mesh workgroups per task workgroup where the stage states one.
  Where it states none, a per-draw budget bounds it, which a host may override per pipeline.
- A draw past its bound drops the excess workgroups, never writing out of bounds, and the `report` profile logs it at the `.sgl` line.
  Drawing the excess in chunks is a later refinement.

**Opt-in until measured, then automatic where the device has no mesh stage.**
The benchmark said the native stage is faster wherever there is one, so `where_missing` became the default and `always` stays a test's.

**Tested and measured where both exist.**
The polyfill is compute and a draw, so it runs on dx12 and vulkan too, beside the native mesh stage on the same device.
There the two images must be identical, equal depths and blending included, and the same pair of runs is the benchmark.

## Still open

- Whether webgpu's per-primitive index reaches a pixel stage, through the feature `primitive_index`, or the polyfill duplicates vertices for per-triangle values instead; to be tried.
- Where a task stage declares its maximum, and its spelling.
- The primitive-order claim above is from memory of both specs, and is checked against their text before a test relies on it.

## Log

- **2026-11-11: the SGL half.**
  `sgl emit --mesh compute|vertex` writes both halves of a mesh entry point over `sg_mesh_output` and `sg_mesh_input`, CHK-524.
- **2026-11-11: drawn on webgpu.**
  `ctx.set_mesh_polyfill(...)` opts in, slib acquires both halves, and sg's webgpu backend dispatches and draws them.
  `tests/pipeline/mesh-polyfill-test.cc` passes under node (Dawn) and deno (wgpu), the primitive order and an unused slot included.
- **2026-11-11: drawn on dx12 and vulkan, and measured.**
  The same tests pass on dx12, dx12 native-DXIL, vulkan and vulkan native-SPIR-V, five times over.
  On an Arc B580 the polyfill costs +41% over the native stage on dx12 and +17% on vulkan, for 400 000 triangles a frame.
- **2026-11-11: automatic where the device has no mesh stage.**
  `where_missing` is the default: it reaches only a device the native stage cannot run on, webgpu today, and `always` stays a test's.
