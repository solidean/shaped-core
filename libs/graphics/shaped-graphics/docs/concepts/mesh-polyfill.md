# The mesh polyfill

**A device without a mesh stage still draws a mesh-shaded pipeline: a compute pass writes what the mesh stage would have handed the rasterizer, and an ordinary draw of it follows.**
SGL writes both halves from the one mesh entry point, by CHK-524 of [checking.md](../../../shaped-graphics-language/docs/spec/semantics/checking.md).
sg draws them through the `draw_mesh_tasks` a native pipeline takes.
The host sees no difference beyond the feature it no longer needs.

This page is the protocol both sides keep.
[stage-polyfills.md](../../../shaped-graphics-language/docs/spec/incubator/stage-polyfills.md) is why it is shaped this way.
The decisions behind it are [feat-sgl-mesh-task-polyfill](../../../shaped-graphics-language/docs/issues/feat-sgl-mesh-task-polyfill.md)'s.

## Status

SGL writes both halves, for a mesh stage with no task stage and no values per primitive.
sg draws them on webgpu; dx12 and vulkan refuse a polyfilled pipeline until their draw lands, which the benchmark against their native stage waits on.

**A polyfill is opt-in until that benchmark has run.**
`ctx.set_mesh_polyfill(sg::polyfill_use::where_missing)` polyfills where the device has no mesh stage, and `always` everywhere, which is how a test compares the two on one device.
shaped-shader-library reads it when it describes a mesh-shaded pipeline, and acquires the two halves in place of the mesh stage.

## The words

One buffer of 32-bit words holds every workgroup's output, in fixed slots, so the draw keeps the primitive order the native stage has.
Workgroup `g` of the draw's grid, numbered `x + nx * (y + ny * z)`, owns `group_words` words from `g * group_words`:

| words | hold |
|---|---|
| 0 | the vertex count `set_counts` stated |
| 1 | the primitive count `set_counts` stated |
| 2 to `2 + K * M` | each primitive slot's `K` corner indices, 3 for triangles and 2 for lines |
| then `N * vertex_words` | each vertex slot, one word a scalar leaf of the vertex struct, in field order |

A leaf is its bits, a bool 0 or 1, and a `half` widened to a float first, as a debug record holds one.
`K`, `M` (`max_primitives`), `N` (`max_vertices`), `vertex_words` and `group_words` are the shader's `mesh_layout`, which both halves report alike.

## What each half binds

| half | stage | reads | writes |
|---|---|---|---|
| compute | compute, at the mesh stage's workgroup size | the pipeline's own groups, and `sg_dispatch_grid` | `sg_mesh_output` |
| vertex | vertex | `sg_mesh_input` | the vertex struct, to the pixel stage |

`sg_mesh_output` and `sg_mesh_input` are the same buffer, bound twice, at the addresses [reserved-bindings.md](../../../shaped-graphics-language/docs/reserved-bindings.md) gives.
**Each half's layout holds its own slot alone**, `pipeline_layout_description::mesh_polyfill` set to `output` or `input`.
WebGPU refuses one buffer bound writable and read-only in one pass, whichever stage each binding is visible to.
The pipeline's own groups are the same in both, so whatever the host binds for the draw binds for the dispatch too.
The compute half numbers its workgroup from `sg_dispatch_grid.threads`, the threads sg dispatched it in, divided by its workgroup size.

## How sg draws one

A polyfilled pipeline's description holds the compute half beside the vertex half, the layout's `K`, `M` and `group_words`, and no mesh stage.
`draw_mesh_tasks(x, y, z)` then:

1. takes a scratch buffer of `x * y * z * group_words` words for the draw;
2. leaves the rendering scope's pass, since no API dispatches compute inside one;
3. dispatches the compute half in `x * y * z` workgroups, over the groups bound for the draw;
4. reopens the pass, loading what it held, and draws `x * y * z * M * K` vertices of the vertex half, unindexed.

A slot past its workgroup's count stands outside the clip volume, so it draws nothing, and the scratch buffer needs no clearing.
On webgpu one buffer serves every draw, grown as needed, since WebGPU orders each pass after the one before it.
Reopening the pass is a split that `render_pass_splits` counts and no warning reports, which batching every polyfilled draw of a scope into one compute pass ahead of it would remove later.

## Not yet

- A task stage, whose launch count the GPU chooses: a declared maximum per task workgroup, or a per-draw budget a host may override.
- Values per primitive, through `primitive_index` where the device has it, or carried on every corner otherwise.
- `draw_mesh_tasks_indirect`, whose vertex count the GPU then writes.
