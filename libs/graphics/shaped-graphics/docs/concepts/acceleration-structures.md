# Concept: acceleration structures

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [barriers](barriers.md), [memory](memory.md), [bindings](bindings.md).

A ray-tracing **acceleration structure** is an opaque, driver-built spatial index the GPU traverses to
find ray/geometry hits. sg exposes two: a **`blas`** (bottom-level) indexes the triangles or procedural
primitives of one mesh; a **`tlas`** (top-level) indexes a set of *instances*, each placing a `blas` into
the world with a transform. A ray tracer traces against a `tlas`.

Two ideas shape the design: the resources are **plain vocabulary types with no typed wrapper**, and
**creating one is a recorded GPU build**, not a `ctx.*` allocation like every other sg resource.

## `blas` / `tlas` are the vocabulary types — no `raw_`/typed split

[`raw_buffer`](../../src/shaped-graphics/raw_buffer.hh) and `raw_texture` split into a raw resource plus a
typed wrapper (`buffer<T>`, `texture<Traits>`) because they carry a *user-defined element type* worth
layering type safety over. An acceleration structure has none — it is an opaque structure the driver
builds and the tracer reads; there is nothing to type. So there is no `raw_blas`: **`blas` and `tlas` are
the resources directly**, handled as `blas_handle` / `tlas_handle` (`std::shared_ptr<... const>`, shared-
immutable, like every sg resource handle). Each is abstract; a backend subclasses it and owns one opaque
[`accel_structure_storage`](../../src/shaped-graphics/types.hh) buffer plus cheaply-derived stats. That a
backend may represent BLAS and TLAS with the same native object is its business — sg keeps them distinct.

## A BLAS is built from geometry; a TLAS from instances that reference BLASes

A **BLAS** takes one or more geometries — each a vertex buffer (plus an optional index buffer, or an AABB
buffer for procedural primitives), an opaque flag, and an optional per-geometry transform.

A **TLAS** takes *instances*. Each instance names a `blas_handle`, a world transform, and a few small
fields (below). Holding the handle is the ownership edge: **a TLAS instance keeps its BLAS alive**, and the
**BLAS must be fully built before the TLAS that references it is built** — the top-level build reads each
referenced BLAS's storage.

Build-input buffers (vertices, indices, AABBs, transforms) must carry the
[`accel_structure_build_input`](../../src/shaped-graphics/types.hh) usage; the result lives in an
`accel_structure_storage` buffer. Both usages already exist in `buffer_usage`.

## Build inputs are a backend-neutral common denominator

The input vocabulary is deliberately the **intersection of DXR and Vulkan RT**, so the same description
builds on either without reshaping:

- **Vertices are `float3` only.** Half/normalized/integer position formats exist in one API or the other,
  not both; excluded until a capability query justifies them.
- **Geometry is triangles, indexed triangles, or procedural** (AABB list). Per-geometry: an `is_opaque`
  flag and an optional 3×4 row-major transform.
- **Instance fields map 1:1 to `D3D12_RAYTRACING_INSTANCE_DESC` / `VkAccelerationStructureInstanceKHR`:**
  a **3×4 row-major** transform, a **24-bit** `instance_id` (surfaced to the shader as `InstanceID()` /
  `gl_InstanceCustomIndex`), an **8-bit** visibility `mask`, an optional per-instance opaque override, a
  cull mode (front/back/none), and a **24-bit** `hit_group_offset` (`InstanceContributionToHitGroupIndex`).
  The 24-bit fields assert on overflow.

Every constraint here is a portability choice, not a backend limitation to route around.

## Build flags are capabilities fixed at build time

Flags select trade-offs the driver bakes into the structure; they cannot be changed afterward:

- **fast-trace vs fast-build** — optimize traversal speed (default) or build speed.
- **allow-update** — permit later *refit* (cheap rebuild reusing topology). Must be set at build time.
- **allow-compaction** (BLAS only) — enable copying the built structure into a smaller buffer.
- **minimize-memory** (TLAS only) — smaller scratch/result at some build cost.

## Building is a recorded command, so it lives on `cmd.raytracing`

Every other sg resource is created by a `ctx.*` factory that just allocates. An acceleration structure
cannot be: its result size comes from a **prebuild query over the build inputs**, and producing it is GPU
work. Allocation and build are therefore one **recorded** step — and sg never records command work from a
`ctx.*` method. So creation is a command-list op on a new **`cmd.raytracing`** scope (mirroring
[`cmd.compute`](../../src/shaped-graphics/command_list.compute.hh) / `cmd.copy`, and the future home of
trace-rays): `cmd.raytracing.build_blas(...)` / `build_tlas(...)` size and allocate the result buffer,
record the build with **transient** scratch, and return the handle.

The returned handle is **persistent — valid across epochs**. "How long may I use this handle" *is* the
persistent-vs-transient axis (see [memory](memory.md)): persistent structures outlive the epoch that built
them and are rebuilt/refit only when their geometry changes. A **transient (single-epoch) variant may come
later** for structures rebuilt from scratch every frame; it would be a property of the build call's result,
not a separate scope. Build scratch is transient either way.

Ordering is inferred, never hand-synchronized: a build declares
[`accel_write`](../../src/shaped-graphics/backend/resource_access.hh) on the `accel_build` stage over its
storage and `accel_read` over each referenced BLAS; a later trace declares `accel_read` on the
`raytracing` stage. The [barriers](barriers.md) system turns those into the right GPU barriers — the
`accel_*` accesses and stages already exist, and `is_unordered_write` already covers `accel_write`.

## Load-bearing invariants

Preserve these; the rest is tuning:

1. **Inputs stay within the DXR∩Vulkan common denominator** — `float3` vertices, 3×4 row-major transforms,
   24-bit `instance_id`/`hit_group_offset`, 8-bit `mask`. Widen only behind a capability query.
2. **A referenced BLAS is fully built before its TLAS**, and a `tlas` instance holds a `blas_handle` so the
   BLAS outlives every TLAS using it.
3. **One opaque `accel_structure_storage` buffer per structure; scratch is transient** (single-epoch),
   never retained.
4. **Build/refit/trace ordering is inferred** from `accel_build`/`accel_write`/`accel_read` + the
   `raytracing` stage — resources are never hand-synchronized.

## dx12 implementation

- Sizing via `GetRaytracingAccelerationStructurePrebuildInfo` over the translated
  `D3D12_RAYTRACING_GEOMETRY_DESC` array (BLAS) or the packed
  `D3D12_RAYTRACING_INSTANCE_DESC` array (TLAS); the build is
  `ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure`.
- Instances pack into `D3D12_RAYTRACING_INSTANCE_DESC`: `InstanceID:24`, `InstanceMask`,
  `InstanceContributionToHitGroupIndex:24`, `Flags` (opaque override + cull), and the 3×4 `Transform`.
- Barriers come free: [`dx12_barrier.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_barrier.cc)
  already maps `accel_build` → `BUILD_RAYTRACING_ACCELERATION_STRUCTURE` sync, `raytracing` → `RAYTRACING`
  sync, and `accel_read`/`accel_write` → the AS read/write barrier accesses. **Only the AS storage buffers
  are acceleration structures** — the build declares `accel_write` on its result and `accel_read` on each
  referenced BLAS. Build *scratch* is a plain UAV (`shader_write`) and the geometry/instance *inputs* are
  ordinary reads (`shader_read`); the enhanced-barrier AS access bits are illegal on non-AS buffers.
- **AS storage buffers are created in — and stay in — the `RAYTRACING_ACCELERATION_STRUCTURE` resource
  state** (with `ALLOW_UNORDERED_ACCESS`), not the usual `COMMON`: D3D12 forbids transitioning an AS
  resource, and both the enhanced AS barriers and `BuildRaytracingAccelerationStructure` require that state.
  This is the one buffer usage that overrides the backend's default COMMON creation
  ([`dx12_buffer.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_buffer.cc)).

The **vulkan** backend stubs the path until its own raytracing milestone: `to_vk_buffer_usage` does not yet
map the `accel_structure_*` usages (nor add the buffer device address they need), and placed allocations —
which a future transient variant would want — aren't implemented yet.

## What's implemented today vs deferred

**Today:** the single-shot build path is implemented. `sg::blas` / `sg::tlas` +
`blas_handle`/`tlas_handle`, the input vocabulary (`blas_triangles`, `blas_aabbs`, `tlas_instance`,
`accel_build_flags`, `instance_cull_mode`, `index_format`), and the `cmd.raytracing` scope
(`build_blas` for triangles and procedural AABBs, `build_tlas`, `is_supported()`) all exist. **dx12** is the
reference realization (prebuild-sized result + transient scratch, `BuildRaytracingAccelerationStructure`,
gated on `D3D12_RAYTRACING_TIER`); it runs on WARP. **vulkan** stubs the build (`CC_UNREACHABLE`) and reports
`is_supported() == false` until its raytracing milestone. The supporting vocabulary and dx12 barrier
translation were already in place.

**The trace side is in.** A `tlas` binds as a shader resource via the `acceleration_structure` binding-type
and view kind (inline `RayQuery` in a compute dispatch), and the full DXR pipeline path —
`raytracing_pipeline` (a DXR state object), `raytracing_shader_table`, and `cmd.raytracing.dispatch_rays` —
runs a raygen→miss→closest-hit trace on WARP. See
[raytracing-pipeline.md](raytracing-pipeline.md).

**Deferred** (see [TODO.md](../TODO.md)):

- the **transient (single-epoch) variant** and the **refit/update + compaction** runtime path;
- a **dedicated shader-table buffer usage** — the table is backed by a plain readable buffer for now
  (`types.hh` reserves `shader_binding_table` as future work);
- **local root signatures** (records currently store only a 32-byte shader identifier — one global root
  signature).

## See also

- [types.hh](../../src/shaped-graphics/types.hh) — `buffer_usage::accel_structure_storage` / `accel_structure_build_input`.
- [resource_access.hh](../../src/shaped-graphics/backend/resource_access.hh) — the `accel_*` access flags and stages.
- [barriers](barriers.md) — how AS build/trace accesses are tracked and ordered.
- [memory](memory.md) — the persistent-vs-transient lifetime axis the handle sits on.
- [bindings](bindings.md) — the future `acceleration_structure` binding used to trace against a `tlas`.
- [cheat-sheet](../../cheat-sheet.md) — the AS API at a glance (once it lands).
