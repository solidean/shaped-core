# Concept: raytracing pipeline + shader table

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [acceleration-structures](acceleration-structures.md), [bindings](bindings.md), [caches](caches.md).

Inline ray tracing (`RayQuery`) traces against a `tlas` bound as a shader resource inside an ordinary
compute dispatch — see [acceleration-structures](acceleration-structures.md). The **full DXR path** is
different: a dedicated `raytracing_pipeline` (a state object of raygen / miss / hit / callable shaders) is
dispatched through a `raytracing_shader_table` with `cmd.raytracing.dispatch_rays`, and the GPU's
fixed-function traversal invokes the right shader per ray.

Two ideas shape the design: a **two-phase handle → index model** connects the pipeline and the table, and
records hold **only a shader identifier** — one global root signature, no per-record data.

## The pipeline mirrors compute_pipeline, but is a state object

A [`raytracing_pipeline`](../../src/shaped-graphics/raytracing_pipeline.hh) is built from a
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

Registering a shader in the pipeline returns a **handle** (`raygen_shader_handle`, `miss_shader_handle`,
`hit_shader_handle`, `callable_shader_handle`) — its slot in the pipeline. Adding that handle to a
[`raytracing_shader_table`](../../src/shaped-graphics/raytracing_shader_table.hh) returns an **index**
(`raygen_index`, …) — its slot in the *table*, which is what HLSL `TraceRay` / `dispatch_rays` address at
launch. The table maps handle → index, so the same pipeline can back several tables with different layouts.

```
raytracing_pipeline_description        raytracing_shader_table
  add_raygen_shader(shader)  ─► handle    add_raygen_shader(handle) ─► index
  add_miss_shader(shader)    ─► handle    add_miss_shader(handle)   ─► index
  add_hit_shader(hit_shader) ─► handle    add_hit_shader(handle)    ─► index
```

## "Shader table", not "SBT" — and why it holds only an identifier

The type is named `raytracing_shader_table` (the friendly name), not "SBT". Each record stores **only** the
backend's 32-byte shader identifier — no local root arguments. That is deliberate: local-record data has no
portable HLSL→SPIR-V spelling across dx12/vulkan, and the table is a hot path. Resources are bound the same
way as compute — through the pipeline's one **global** root signature (`cmd.raytracing.bind_group`, which
binds through the compute root signature). Local root signatures are deferred.

The dx12 table lays out four sections (raygen / miss / hit / callable) with the DXR alignments — records at
32 bytes, sections at 64 — copies the pipeline's stored identifiers by handle index, and uploads them into
a shader-readable buffer, exposing the GPU address ranges `DispatchRays` needs. (It is backed by a plain
readable buffer for now; `types.hh` reserves a dedicated `shader_binding_table` usage as future work.)

## dispatch_rays reuses the compute bind/hazard machinery

[`cmd.raytracing.dispatch_rays(table, raygen, width, height, depth)`](../../src/shaped-graphics/command_list.raytracing.hh)
records the trace. In dx12 it binds the state object with `SetPipelineState1`, binds groups through the
compute root signature, then runs the same **declare-hazards → flush → op** rhythm as `compute_dispatch`
at `pipeline_stage_flags::raytracing`: a bound `tlas` surfaces as `accel_read`, and the shader-table buffer
is declared `shader_read`, before `ID3D12GraphicsCommandList4::DispatchRays`.

## See also

- [acceleration-structures](acceleration-structures.md) — building the `blas`/`tlas` a trace runs against.
- [bindings](bindings.md) — the `acceleration_structure` binding and the group/layout bind path.
- [caches](caches.md) — the async, content-addressed pipeline cache the RT pipeline slots into.
- [cheat-sheet](../../cheat-sheet.md) — the RT pipeline + shader table + `dispatch_rays` API at a glance.
