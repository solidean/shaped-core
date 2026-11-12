# The mesh polyfill

**A device without a mesh stage still draws a mesh-shaded pipeline: a compute pass writes what the mesh stage would have handed the rasterizer, and an ordinary draw of it follows.**
SGL writes both halves from the one mesh entry point, by CHK-524 of [checking.md](../../../shaped-graphics-language/docs/spec/semantics/checking.md).
sg draws them through the `draw_mesh_tasks` a native pipeline takes.
The host sees no difference beyond the feature it no longer needs.

This page is the protocol both sides keep.
[stage-polyfills.md](../../../shaped-graphics-language/docs/spec/incubator/stage-polyfills.md) is why it is shaped this way.
The decisions behind it are [feat-sgl-mesh-task-polyfill](../../../shaped-graphics-language/docs/issues/feat-sgl-mesh-task-polyfill.md)'s.

## Status

SGL writes both halves, for a mesh stage with no task stage.
sg draws them on webgpu, dx12 and vulkan; metal has every mesh stage natively and draws none.

**Measured on an Arc B580** (2026-11-11, `tests/pipeline/mesh-polyfill-benchmark.cc`), a frame of 4096 workgroups of 98 triangles each:

| backend | native | polyfilled |
|---|---|---|
| dx12 | 1.33 ms | 1.87 ms, +41% |
| vulkan | 1.39 ms | 1.62 ms, +17% |

**The polyfill is automatic where the device has no mesh stage**, `ctx.mesh_polyfill()`'s default of `where_missing`, decided by the measurements below.
`always` polyfills everywhere, which is how a test compares the two on one device, and `never` refuses a mesh stage the device lacks.
shaped-shader-library reads it when it describes a mesh-shaded pipeline, and acquires the two halves in place of the mesh stage, and the pixel stage that follows them.

## The words

One buffer of 32-bit words holds every workgroup's output, in fixed slots, so the draw keeps the primitive order the native stage has.
Workgroup `g` of the draw's grid, numbered `x + nx * (y + ny * z)`, owns `group_words` words from `g * group_words`:

| words | hold |
|---|---|
| 0 | the vertex count `set_counts` stated |
| 1 | the primitive count `set_counts` stated |
| 2 to `2 + K * M` | each primitive slot's `K` corner indices, 3 for triangles and 2 for lines |
| then `N * vertex_words` | each vertex slot, one word a scalar leaf of the vertex struct, in field order |
| then `M * primitive_words` | each primitive slot's values, `mesh_output[V, P]`'s `P`, the same way; none without a `P` |

A leaf is its bits, a bool 0 or 1, and a `half` widened to a float first, as a debug record holds one.
`K`, `M` (`max_primitives`), `N` (`max_vertices`), `vertex_words`, `primitive_words` and `group_words` are the shader's `mesh_layout`, which both halves report alike.

## Values per primitive

**What a mesh stage writes per primitive rides flat on each corner of it**, since the unindexed draw runs every corner on its own.
The vertex half returns one struct, `sgl_mesh_corner`: the vertex struct's members, then the primitive struct's, flat.
Each primitive member then stands at the location CHK-489 gives it, right after the vertex's.
The pipeline's pixel stage is compiled after the vertex half too: it takes that struct, and makes its own two from it before its body.
So no target marks a pixel input per primitive, which a vertex stage cannot feed, and no device needs `primitive_index`, which webgpu has none of.

## What each half binds

| half | stage | reads | writes |
|---|---|---|---|
| compute | compute, at the mesh stage's workgroup size | the pipeline's own groups, and `sg_dispatch_grid` | `sg_mesh_output` |
| vertex | vertex | `sg_mesh_input` | the vertex struct, to the pixel stage, or `sgl_mesh_corner` with values per primitive |

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
On dx12 and vulkan each draw takes a transient buffer of its own, and the barrier tracker orders its compute write before its vertex read.
dx12 dispatches inside the scope, which binds its targets without a render pass; vulkan suspends its rendering instance around the dispatch.
Reopening the pass is a split that `render_pass_splits` counts and no warning reports, which batching every polyfilled draw of a scope into one compute pass ahead of it would remove later.

## A geometry stage on the same words

**A device without a geometry stage draws one the same way** (CHK-525): metal and webgpu, which have none.
SGL writes the compute half from the pipeline's vertex stage and the geometry stage together, and the vertex half is this page's.
A primitive takes a workgroup's place: thread `t` of the dispatch is primitive `t % primitives` of instance `t / primitives`, and owns `group_words` words from `t * group_words`.
`max_vertices` is the stage's own, and `max_primitives` is what one strip of that many vertices cuts into: `max_vertices - 2` triangles, or `max_vertices - 1` lines.

The compute half assembles each primitive from the draw's own buffers, which sg binds as words:

| entry | holds |
|---|---|
| `sg_geometry_draw[0]` | first vertex, or first index of an indexed draw; primitives an instance; index kind, 0 none, 1 16-bit, 2 32-bit; the vertex offset added to each index |
| `sg_geometry_draw[1]` | first instance; the index buffer's offset, in indices; two words unused |
| `sg_geometry_draw[2]` | each vertex buffer's offset, in words, in slot order |
| `sg_geometry_indices` | the index buffer bound for the draw; a 16-bit index is half a word, the low half first |
| `sg_geometry_vertices0` to `3` | the vertex buffers bound, in slot order |

A vertex buffer's element is its `@vertex struct` stream as the host packs it: each member in order, a word a scalar leaf, a packed `@format` one word.
So its stride is in the shader, and only the offsets are the draw's.

`draw` and `draw_indexed` of such a pipeline then:

1. take a scratch buffer of `primitives * instances * group_words` words;
2. leave the rendering scope's pass;
3. dispatch the compute half over `primitives * instances` threads, with the draw's buffers and the groups bound for it;
4. reopen the pass and draw `primitives * instances * max_primitives * corners` vertices of the vertex half, unindexed.

A list topology alone: a strip's primitives share corners, which no fixed slot assigns yet.

**Opt-in until measured**: `ctx.geometry_polyfill()` is `never` by default, and `where_missing` or `always` asks for it.
slib then acquires the geometry stage's two halves in place of the geometry stage, the compute half naming the pipeline's vertex stage, and leaves the pipeline no vertex input.
metal draws it, `draws_geometry_polyfill`, with the draw's buffers bound past every vertex-input slot and their offsets folded into the addresses.
A slot the draw has no buffer for, the index buffer of a draw that is not indexed, takes the draw's own values, since Metal's validation wants every declared buffer bound.
dx12, vulkan and webgpu draw none yet, so the benchmark against the native stage that decides `where_missing` has not run.

## Not yet

- A task stage, whose launch count the GPU chooses: a declared maximum per task workgroup, or a per-draw budget a host may override.
- `draw_mesh_tasks_indirect`, whose vertex count the GPU then writes.
