# The mesh polyfill

**A device without a mesh stage still draws a mesh-shaded pipeline: a compute pass writes what the mesh stage would have handed the rasterizer, and an ordinary draw of it follows.**
SGL writes both halves from the one mesh entry point, by CHK-524 of [checking.md](../../../shaped-graphics-language/docs/spec/semantics/checking.md).
sg draws them through the `draw_mesh_tasks` a native pipeline takes.
The host sees no difference beyond the feature it no longer needs.

This page is the protocol both sides keep.
[stage-polyfills.md](../../../shaped-graphics-language/docs/spec/incubator/stage-polyfills.md) is why it is shaped this way.
The decisions behind it are [feat-sgl-mesh-task-polyfill](../../../shaped-graphics-language/docs/issues/feat-sgl-mesh-task-polyfill.md)'s.

## Status

SGL writes both halves, and a task stage's two compute forms before them.
sg draws them on webgpu, dx12 and vulkan; metal has every mesh and task stage natively and draws none.

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

## A task stage before it

**A task stage is three compute forms ahead of the draw, and the draw becomes indirect**, since what the task workgroups launch is the GPU's to count.
The order stays the native one, task workgroup by task workgroup and then by mesh workgroup within each, by a scan over what each launched rather than an append.

A second buffer of words holds the task workgroups' records behind a header:

| words | hold |
|---|---|
| 0 | the task workgroups that hold a record, which the scan writes |
| 1 | the mesh workgroups launched, cut to the slots the draw holds |
| 2 to 4 | the mesh pass's dispatch arguments, `launched, 1, 1` |
| 5 to 8 | the draw's arguments, `launched * M * K, 1, 0, 0` |
| 9 to 11 | the draw's task workgroups along each axis, which sg writes before the draw |
| then `record_words` a task workgroup | the grid of mesh workgroups it launches, the first of them among all launched, and its payload's words |

A task workgroup's launch is cut to `@task(…, meshes = N)`'s `N` where the stage states one.
The draw then holds slots for `N` mesh workgroups a task workgroup; where it states none, for `mesh_polyfill_description::launch_budget`, 4096 unless the host sets it.
Either is cut to the 65535 one dispatch dimension holds, and a launch past the slots is dropped.
`record_words`, `N` and the budget are the shader's `task_layout` and the description's, which sg sizes the buffer by.

`draw_mesh_tasks(x, y, z)` of such a pipeline then writes `x, y, z` into words 9 to 11 and dispatches, in one pass:

1. the task stage's compute form, in `x * y * z` workgroups numbered by words 9 to 11, its first invocation writing its record in place of `dispatch_mesh`;
   a workgroup past the records `sg_dispatch_grid.threads.x` says the draw holds writes none;
2. the scan, one invocation, which counts the task workgroups that hold a record, cut to `threads.y`;
   it then writes each record's first launched, the count launched cut to the slots `threads.x` says the draw holds, and both arguments;
3. the mesh stage's compute half, indirectly by words 2 to 4; workgroup `g` finds the last record whose first is at most `g`, and reads its workgroup id within that grid and its payload there;
4. then the vertex half, indirectly by words 5 to 8.

| form | writes | reads |
|---|---|---|
| task stage's compute form | `sg_task_output` | the pipeline's groups, `sg_dispatch_grid` |
| scan | `sg_task_output` | `sg_dispatch_grid` |
| mesh stage's compute half | `sg_mesh_output` | the pipeline's groups, `sg_task_input` |

`sg_task_input` is read-only, which keeps a launched mesh stage's workgroup id uniform in WGSL, as `@builtin(workgroup_id)` is: a load of read-only storage at a uniform index is.
So a barrier under a branch on it compiles on webgpu as it does natively, which `halves` and `culled_column` of the tests' `meshes.sgl` hold.

The first two take a layout of `mesh_polyfill_slots::task`, the third one of `launched`.

## An indirect draw

**`draw_mesh_tasks_indirect` of a polyfilled pipeline draws each record of its arguments as the draws above**, its grid the GPU's.
Its slots and the task workgroups it holds records for are then `launch_budget` each, cut to 65535, and what the GPU asks past them is dropped.
Behind a task stage, sg copies the record's grid into words 9 to 11 and dispatches the task stage's compute form by the arguments themselves, since a dispatch reads none from a buffer it writes.
With no task stage, the draw is one task workgroup of no payload: sg writes `1, 1, 1` into words 9 to 11 and copies the record's grid into the one record, of 4 words.
The scan and the mesh stage's compute half read it as they would a task stage's.
That compute half is SGL's `launched` form, which slib acquires beside the `compute` one for every polyfilled mesh stage with no task stage, as `mesh_polyfill_description::launched_shader`.
A buffer of `buffer_usage::indirect_command_buffer` is a copy source on every backend for this, without `copy_src`.

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

## The tessellation stages, by D3D's tessellator

**A device without tessellation draws it by compute passes** (CHK-527): metal, whose Metal 4 has none, and webgpu.
SGL writes every pass, and the tessellator is D3D's, ported into its prelude, so the points and their order are D3D's.
A draw's vertices are its patches' control points, `control_points` a patch, and the passes read them from the `sg_geometry_*` entries as a geometry stage's compute half does.

| entry | holds |
|---|---|
| `sg_tessellation_draw[0]` | patches an instance; instances; partitioning, 0 integer, 1 fractional even, 2 fractional odd; D3D's output topology, 0 `triangle_cw`, 1 `triangle_ccw`, 2 lines |
| `sg_tessellation_draw[1]` | the domain's edges, 2 isolines, 3, 4; control points a patch; words a patch; the largest factor, a float's bits |
| `sg_tessellation_controls` | `patch_words` words a patch: its control points as the vertex stage returned them, then the factors struct as the control stage did |
| `sg_tessellation_patches` | 8 words a patch: the tessellator's 6 factors as floats, then its first point and its first index |
| `sg_tessellation_points` | 3 words a point: u and v in 16.16, then its patch |
| `sg_tessellation_indices` | the draw's index buffer, each index a point |
| `sg_tessellation_args` | the indexed draw's 5 arguments, the evaluation pass's 3 workgroup counts, then the points in all |

`draw` and `draw_indexed` of such a pipeline then:

1. take scratch buffers sized for every patch at the largest factor, `max_factor`, 64 by default;
2. leave the rendering scope's pass;
3. dispatch the control pass over `patches * instances` threads, which runs the vertex stage on each control point and the control stage on the patch;
4. count each patch's points and indices, scan them in patch order in one invocation, then tessellate each patch, a thread a patch;
5. dispatch the evaluation pass by the scan's workgroup count, a thread a point, which writes the mesh polyfill's words, `vertex_words` a point;
6. reopen the pass and draw the vertex half by the tessellator's indices, indirectly, by the scan's count.

**Opt-in until measured**: `ctx.tessellation_polyfill()` is `never` by default.
slib then acquires the evaluation stage's vertex half as the vertex stage, both stages' compute halves, and the prelude's three passes through the control stage's source.
metal draws it, `draws_tessellation_polyfill`; dx12, vulkan and webgpu draw none yet.

## Not yet

- A launch dropped past the slots is silent; the decision is that the `report` profile logs it at the `.sgl` line.
- The scan is one invocation, a loop over the task workgroups, which a parallel scan replaces once a benchmark of many task workgroups says it matters.
>>>

- The task stage is drawn this way wherever the mesh stage is, `where_missing` by default, with no benchmark of its own yet.
