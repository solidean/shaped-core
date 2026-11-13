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
- **2026-11-12: values per primitive, flat on each corner.**
  The open question of `primitive_index` is settled without it: sg reports no `primitive_index` on webgpu, and the pixel stage would have to know the mesh stage's layout.
  The unindexed draw already runs every corner on its own, so each corner carries its primitive's values, flat, at no cost in words.
  The vertex half returns one struct of both, and the pixel stage, compiled after it, takes that struct and makes its own two.
  So neither stage is marked per primitive, which vulkan's SPIR-V would otherwise refuse to link.
  `tinted` draws the native stage's image on webgpu (Dawn and wgpu), dx12, dx12 native-DXIL, vulkan and vulkan native-SPIR-V.
- **2026-11-12: the task stage.**
  The maximum is spelled `@task(32, meshes = 64)`, after `@mesh`'s `vertices = N`; a host's budget per pipeline, 4096 by default, bounds a draw of one that states none.
  A task workgroup's first invocation writes a record: its launch, its grid and its payload.
  A scan of one invocation numbers what was launched, cut to the slots the draw holds, and writes the indirect arguments of the mesh pass and of the draw.
  A launched mesh workgroup finds its task workgroup by a binary search over the records, so the order is the native one.
  `launched` and `culled` (four task workgroups launching 0, 1, 2 and 0) draw the native stages' images on webgpu (Dawn and wgpu), dx12, dx12 native-DXIL, vulkan and vulkan native-SPIR-V.
  Still open of it: the `report` profile's log of a dropped launch, a parallel scan once measured, and WGSL's uniformity of a launched mesh stage's workgroup id.
- **2026-11-12: indirect draws.**
  `draw_mesh_tasks_indirect` draws a polyfilled pipeline, each record of its arguments one draw, with the budget renamed `launch_budget` for the slots and task records it holds.
  The task grid moved into the record buffer's header, which sg writes on a direct draw and copies from the arguments on an indirect one; the scan now counts launches from each record's grid.
  With no task stage, an indirect draw is one record of no payload, launching SGL's new `launched` form, which slib acquires for every polyfilled mesh stage.
  `split` and `culled` drawn indirectly match the native images on webgpu (Dawn and wgpu), dx12, dx12 native-DXIL, vulkan and vulkan native-SPIR-V.
- **2026-11-12: the WGSL uniformity item was a false alarm.**
  A launched mesh stage reads its workgroup id from `sg_task_input`, which is `var<storage, read>`, and WGSL counts a load of read-only storage at a uniform index as uniform.
  Dawn's Tint compiles a barrier under a branch on it, and refuses the same branch on a `read_write` load; wgpu's validator accepts both, so it could not have told.
  A barrier under a branch on the payload is SGL's own `non-uniform-control-flow` on every target, so it never reaches WGSL.
  `halves` and `culled_column` now hold a barrier under such a branch, which every webgpu run compiles.
