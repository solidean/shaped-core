# Concept: raytracing pipeline + shader table

sg has two ways to trace a ray, and they are two features rather than one.
**A ray query** (`sg::feature::ray_query`) traces from inside an ordinary stage, against a `tlas` bound as a shader resource — see [acceleration-structures](acceleration-structures.md).
**The ray-tracing pipeline** (`sg::feature::raytracing_pipeline`) is the full DXR path.
A dedicated `raytracing_pipeline` of raygen / miss / hit / callable shaders is dispatched through a `raytracing_shader_table` with `cmd.raytracing.dispatch_rays`.
Traversal then invokes the right shader per ray.

Two ideas shape the pipeline's design.
A **two-phase handle → index model** connects the pipeline and the table.
And records hold **only a shader identifier** — one global root signature, no per-record data.

## Two features, because webgpu has one of them

| backend | `ray_query` | `raytracing_pipeline` |
|---|---|---|
| dx12 | `D3D12_RAYTRACING_TIER_1_1` | `D3D12_RAYTRACING_TIER_1_0` |
| vulkan | one probe answers both | one probe answers both |
| metal | every device above the Metal 4 floor | every device above the Metal 4 floor |
| webgpu | **emulated**: a software polyfill | no |

Vulkan asks one question for both because DXC writes the `RayQueryKHR` capability into every ray-tracing SPIR-V module, so a device with the pipeline and no ray query could load none of them.
[writing-a-backend](../writing-a-backend.md) records how that was found.

**`ctx.implementation_of(feature)` says how a context provides a feature**: `native`, `emulated` in software on top of the device, or `absent` where `supports` says no.
Only webgpu's `ray_query` is `emulated` today.
It is a question about cost, for a caller choosing an algorithm; a shader never asks it, since both run the same source.
`cmd.raytracing.is_supported()` is true where either feature is, which is where acceleration structures build.

The webgpu polyfill keeps every BLAS and TLAS in one storage buffer per context and traverses it from SGL's prelude.
[backends/webgpu/readme.md](../../backends/webgpu/readme.md#ray-queries) is what the backend does.
SGL's [raytracing-polyfill.md](../../../shaped-graphics-language/docs/raytracing-polyfill.md) is the layout both sides agree on.
There is no pipeline polyfill: a path tracer on webgpu is written against ray queries.

## The pipeline mirrors compute_pipeline, but is a state object

A [`raytracing_pipeline`](../../src/shaped-graphics/raytracing/raytracing_pipeline.hh) is built from a
`raytracing_pipeline_description`: a `pipeline_layout` (its global root signature) plus shaders grouped by
category — `raygen_shaders`, `miss_shaders`, `callable_shaders`, and `hit_shaders` (each a `hit_shader` of
optional closest-hit / any-hit / intersection). Unlike `compute_pipeline_description`, which references one
shader, this **owns** its shaders (a pipeline combines several), so an async build on a worker is safe.

Each ray-tracing shader is its own single-entry `lib_6_x` blob — sg's `compiled_shader` is already
single-entry, so there is **no separate "shader library" type**. The dx12 backend assembles one
`ID3D12StateObject`: it deduplicates the DXIL libraries by bytecode pointer, renames their exports so the
same entry name can appear in two libraries, builds a hit group per `hit_shader` (procedural iff an
intersection shader is present, else triangles), and attaches a single shader-config, pipeline-config, and
**global** root signature that apply to every export.

Like `compute_pipeline`, it routes through the [pipeline cache](caches.md): `ctx.cached
.acquire_raytracing_pipeline` memoizes on the shaders' content + layout identity + limits and builds
asynchronously; `ctx.uncached.create_raytracing_pipeline` is the synchronous escape hatch.

## Two phases: a handle registers, an index places

Registering a shader on the `raytracing_pipeline_description` returns a **handle**: its slot in the pipeline.
One per category — `raygen_shader_handle`, `miss_shader_handle`, `hit_shader_handle`, `callable_shader_handle`.
Adding that handle to a [`raytracing_shader_table_description`](../../src/shaped-graphics/raytracing/raytracing_shader_table.hh) returns an **index** (`raygen_index`, …), its slot in the *table*.
The index is what HLSL `TraceRay` and `dispatch_rays` address at launch.
The table maps handle → index, so the same pipeline can back several tables with different layouts.

```
raytracing_pipeline_description        raytracing_shader_table
  add_raygen_shader(shader)  ─► handle    add_raygen_shader(handle) ─► index
  add_miss_shader(shader)    ─► handle    add_miss_shader(handle)   ─► index
  add_hit_shader(hit_shader) ─► handle    add_hit_shader(handle)    ─► index
                                          add_hit_row(handles)      ─► hit_row
```

## Hit rows: one record per ray type

A shader table's `ray_count` says how many ray types trace through it.
`add_hit_row` appends `ray_count` consecutive hit records, one per ray type, and returns a `hit_row` whose value is the first record's index.
`table.offset_of(row)` is what an instance takes as its `hit_group_offset`.
A trace of ray type r then passes r as its ray contribution and `ray_count` as its geometry multiplier, so geometry g of the instance reads record `hit_group_offset + g * ray_count + r`.
Rows and single `add_hit_shader` records append to the same list and mix freely, which keeps the handle and index API a hand-written HLSL table uses.

A BLAS takes the same number at build time: `build_blas(..., hit_record_stride)` is the records one of its geometries spans.
Only metal needs it, because metal bakes the geometry term into the structure; dx12 and vulkan take the multiplier per `TraceRay`.

A hit group with no shaders at all is valid.
It is a triangle group that accepts every hit and runs nothing, which is what a row holds for a ray type its hit group leaves empty.

## A mismatched hit group is logged at dispatch, under the portability checks

A triangle BLAS must not reach a procedural hit group and a procedural BLAS must, and no backend checks it for you.
With `ctx.portability_checks()` on, `dispatch_rays` walks every instance of every bound TLAS and checks each record it reaches.
Each must exist and match the BLAS's kind, and a BLAS of more than one geometry must have been built with the table's ray count as its stride.
The check assumes each trace's geometry multiplier is that ray count, which an SGL-generated table's traces are.
A hand-written shader passing another multiplier, such as `TraceRay(…, ray_type, 0, …)` sharing one record across geometries, keeps the checks off.
A mismatch **logs an error rather than asserting**, since the hit groups come from shaders that hot reload can change under a running program.
It is logged once per table and TLAS in a command list, and the dispatch still runs.

The check reads only what sg recorded on the CPU side, and records it only while the checks are on.
A TLAS keeps its instances' BLAS, offset and mask, and a binding group the TLASes it binds, so both must be made after the checks were turned on.
A staging group records the TLASes it is set to once it was made with the checks on, and hands them to each snapshot.
A BLAS always keeps its kind and stride, and a pipeline its hit groups' kinds, because they cost nothing per operation.

## "Shader table", not "SBT" — and why it holds only an identifier

The type is named `raytracing_shader_table` — the friendly name, not "SBT".
Each record stores **only** the backend's 32-byte shader identifier, with no local root arguments.
That is deliberate: local-record data has no portable HLSL→SPIR-V spelling across dx12 and vulkan, and the table is a hot path.
Resources bind the same way as in compute, through the pipeline's one **global** root signature — `cmd.raytracing.bind_group` binds through the compute root signature.
Local root signatures are deferred.

The dx12 table lays out four sections (raygen / miss / hit / callable) with the DXR alignments: records at 32 bytes, sections at 64.
It copies the pipeline's stored identifiers by handle index, uploads them into a shader-readable buffer, and exposes the GPU address ranges `DispatchRays` needs.
That plain readable buffer is the storage today, and the table's own abstraction is the open item — [types.hh](../../src/shaped-graphics/types.hh) rules an SBT out of `buffer_usage` deliberately.

## dispatch_rays reuses the compute bind/hazard machinery

[`cmd.raytracing.dispatch_rays(table, raygen, width, height, depth)`](../../src/shaped-graphics/command_list/raytracing.hh) records the trace.
In dx12 it binds the state object with `SetPipelineState1` and binds groups through the compute root signature.
It then runs the same **declare-hazards → flush → op** rhythm as `compute_dispatch`, at `pipeline_stage_flag::raytracing`.
A bound `tlas` surfaces as `accel_read` and the shader-table buffer is declared `shader_read`, before `ID3D12GraphicsCommandList4::DispatchRays`.

## metal maps it onto a compute pipeline, because a raygen shader is the kernel

There is no MTL4 ray-tracing pipeline, and this is the sharpest fork in the whole surface.
DXR hands the driver a set of shaders and lets it schedule them.
Metal dispatches an ordinary compute kernel that calls `intersector` itself, with function tables supplying what traversal and the kernel call back into.
So a raygen shader is not something a pipeline dispatches there — it **is** the kernel.

- A `raytracing_pipeline` builds **one MTL4 compute pipeline per registered raygen shader**, dynamically linked with every hit, miss and callable function.
- A `raytracing_shader_table` becomes **four** Metal tables rather than one buffer of records.
  MSL's `visible_function_table<T>` is typed by the function signature, so miss, closest-hit and callable functions cannot share a table.
  Each of sg's index spaces therefore gets one, and the indices are used verbatim.
- One sg hit group splits across both kinds: `intersection` and `any_hit` run during traversal and go in the intersection function table, while `closest_hit` is a visible function the kernel calls.
  **Metal runs exactly one function during traversal**, so a *procedural* group may carry an intersection function or an any-hit but not both, and metal refuses the pair rather than dropping one.
  Fold the any-hit's decision into the intersection function, which is where it already decides what the ray hit.
- **All three of DXR's hit-index contributions map, for a ray type fixed at each call site.**
  The instance's `InstanceContributionToHitGroupIndex` is the instance descriptor's `intersectionFunctionTableOffset`.
  The geometry contribution is each geometry descriptor's own offset, which metal sets to its geometry index times the BLAS's `hit_record_stride`.
  `RayContributionToHitGroupIndex`, DXR's per-`TraceRay` term, becomes a choice of table: the shader table builds one intersection table per ray type, and table r's slot s holds hit record s + r.
  A kernel that traces ray type r with table r reaches record `hit_group_offset + g * stride + r`, exactly as DXR does.
  That closes the gap for pipelines that trace a constant ray type per call site, which is what SGL generates.
  A ray contribution computed at run time still has no counterpart, so a shader ported from HLSL that varies it dynamically selects a different function on metal than on dx12.
- **The kernel finds a closest hit's record itself, and needs each instance's `hit_group_offset` to do it.**
  Traversal applies the offset to the intersection table, but Metal's intersection result names the instance and not the offset it carried.
  So a metal TLAS keeps its instances' offsets in a buffer of its own, and `dispatch_rays` binds it where the kernel reads `offsets[instance] + g * stride + r`.
  **One TLAS per dispatch follows from that**: the buffer bound is the first bound TLAS's, and a dispatch binding two logs a warning.
- **An empty closest-hit slot is a valid table entry and not a function**, so a kernel may not call it.
  DXR skips a record without a closest hit; a kernel that calls every record's closest hit unconditionally needs something in each slot.
  slib fills them for an SGL pipeline, as [its ray-tracing doc](../../../shaped-shader-library/docs/raytracing-pipelines.md#what-metal-needs-and-slib-supplies) says.
- `dispatch_rays` selects that raygen's pipeline state, binds the tables through `sg::reserved_binding_group`, and calls `dispatchThreads`.
- `max_recursion_depth` becomes Metal's `maxCallStackDepth`, which sizes the stack for indirect calls and defaults to 1.
  Recursion itself is supported — a visible function may trace and may call back through a table — so the field is honoured rather than capped.
  What cannot recurse is traversal: an intersection or any-hit function cannot take an acceleration structure at all.

[backends/metal/readme.md](../../backends/metal/readme.md) carries the `[[id(n)]]` assignments, the buffer index, and the rest.

## An SGL pipeline states all of this for you

A `@raytracing pipeline` declaration in SGL knows its ray types, its hit groups and its callables, so slib generates a type that registers the shaders and places the records in one fixed order.
The host then writes no handle or index by hand: it takes `ray_count`, a row per hit group and each row's offset from the generated type.
[slib's raytracing-pipelines.md](../../../shaped-shader-library/docs/raytracing-pipelines.md) is that side.

## See also

- [acceleration-structures](acceleration-structures.md) — building the `blas`/`tlas` a trace runs against.
- [bindings](bindings.md) — the `acceleration_structure` binding and the group/layout bind path.
- [caches](caches.md) — the async, content-addressed pipeline cache the RT pipeline slots into.
- [slib's raytracing-pipelines](../../../shaped-shader-library/docs/raytracing-pipelines.md) — the pipeline and table an SGL declaration generates.
- [cheat-sheet](../../cheat-sheet.md) — the RT pipeline + shader table + `dispatch_rays` API at a glance.
