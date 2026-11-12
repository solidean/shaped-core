# Concept: acceleration structures

A ray-tracing **acceleration structure** is an opaque, driver-built spatial index the GPU traverses to find ray/geometry hits.
sg exposes two.
A **`blas`** (bottom-level) indexes the triangles or procedural primitives of one mesh; a **`tlas`** (top-level) indexes a set of *instances*, each placing a `blas` into the world with a transform.
A ray tracer traces against a `tlas`.

Two ideas shape the design.
The resources are **plain vocabulary types with no typed wrapper**, and **creating one is a recorded GPU build** rather than a `ctx.*` allocation like every other sg resource.

## `blas` / `tlas` are the vocabulary types — no `raw_`/typed split

[`raw_buffer`](../../src/shaped-graphics/resource/raw_buffer.hh) and `raw_texture` split into a raw resource plus a typed wrapper — `buffer<T>`, `texture<Traits>`.
They do so because they carry a *user-defined element type* worth layering type safety over.
An acceleration structure has none: it is an opaque structure the driver builds and the tracer reads, with nothing to type.
So there is no `raw_blas` — **`blas` and `tlas` are the resources directly**, handled as `blas_handle` / `tlas_handle` (`std::shared_ptr<... const>`, shared-immutable like every sg resource handle).
Each is abstract; a backend subclasses it, owns the native structure, and the cheaply-derived stats live on the base.
That a backend may represent BLAS and TLAS with the same native object is its business — sg keeps them distinct.

**The base holds no handle to the structure's memory**, and that is a portability choice rather than an omission.
DXR names a structure by the GPU virtual address of the buffer the driver built it into, so on dx12 the buffer *is* the structure.
Vulkan wraps a `VkAccelerationStructureKHR` around such a buffer.
Metal's `MTL::AccelerationStructure` derives from `MTL::Resource` and is not a buffer at all.
There is no `MTLBuffer` in the caller's hands at any point, and it is named by a `gpuResourceID()` instead.
So "one opaque storage buffer per structure" was a D3D12 fact that had leaked into the portable layer, and `size_in_bytes()` answers the only question it was being asked.
Releasing whatever backs the structure is the backend's, through the `on_expired()` hook.

## A BLAS is built from geometry; a TLAS from instances that reference BLASes

A **BLAS** takes one or more geometries — each a vertex buffer (plus an optional index buffer, or an AABB
buffer for procedural primitives), an opaque flag, and an optional per-geometry transform.

A **TLAS** takes *instances*. Each instance names a `blas_handle`, a world transform, and a few small
fields (below). Holding the handle is the ownership edge: **a TLAS instance keeps its BLAS alive**, and the
**BLAS must be fully built before the TLAS that references it is built** — the top-level build reads each
referenced BLAS.

Build-input buffers — vertices, indices, AABBs, transforms — must carry the [`accel_structure_build_input`](../../src/shaped-graphics/types.hh) usage.
What the result lives in is the backend's, and differs by API — see the paragraph above.

## Build inputs are a backend-neutral common denominator

The input vocabulary is deliberately the **intersection of every supported backend**, so the same description
builds on any of them without reshaping.
It was computed against DXR and Vulkan RT first, and Metal — checked field by field when that backend landed — is no narrower:

- **Vertices are `float3` only.** Half/normalized/integer position formats exist in one API or the other,
  not both; excluded until a capability query justifies them.
- **Geometry is triangles, indexed triangles, or procedural** (AABB list). Per-geometry: an `is_opaque`
  flag and an optional 3×4 row-major transform.
- **Instance fields map 1:1 to `D3D12_RAYTRACING_INSTANCE_DESC` / `VkAccelerationStructureInstanceKHR`:**
  a **3×4 row-major** transform, a **24-bit** `instance_id` (surfaced to the shader as `InstanceID()` /
  `gl_InstanceCustomIndex`), an **8-bit** visibility `mask`, an optional per-instance opaque override, a
  cull mode (front/back/none), and a **24-bit** `hit_group_offset` (`InstanceContributionToHitGroupIndex`).
  The 24-bit fields assert on overflow.
- **A triangle geometry may name an opacity micromap**, behind `sg::feature::opacity_micromap` — see [Opacity micromaps](#opacity-micromaps).
- **A BLAS build takes a `hit_record_stride`**, the shader-table records one of its geometries spans.
  Metal bakes it into each geometry's offset, and dx12 and vulkan ignore it; [raytracing-pipeline](raytracing-pipeline.md#hit-rows-one-record-per-ray-type) says why.

Every constraint here is a portability choice, not a backend limitation to route around.

## Build flags are capabilities fixed at build time

Flags select trade-offs the driver bakes into the structure; they cannot be changed afterward:

- **fast-trace vs fast-build** — optimize traversal speed (default) or build speed.
- **allow-update** — permit later *refit* (cheap rebuild reusing topology). Must be set at build time.
- **allow-compaction** (BLAS only) — enable copying the built structure into a smaller buffer.
  Metal needs no opt-in for this and ignores the flag: `copyAndCompactAccelerationStructure` works on any built structure, where DXR and Vulkan want it declared at build.
  That is Metal asking for less ceremony rather than offering less capability.
- **minimize-memory** (TLAS only) — smaller scratch/result at some build cost.

## Building is a recorded command, so it lives on `cmd.raytracing`

Every other sg resource is created by a `ctx.*` factory that just allocates.
An acceleration structure cannot be: its result size comes from a **prebuild query over the build inputs**, and producing it is GPU work.
Allocation and build are therefore one **recorded** step, and sg never records command work from a `ctx.*` method.
So creation is a command-list op on the **`cmd.raytracing`** scope, which also carries `dispatch_rays`.
`cmd.raytracing.build_blas(...)` / `build_tlas(...)` size and allocate the structure, record the build with **transient** scratch, and return the handle.

The returned handle is **persistent — valid across epochs**.
"How long may I use this handle" *is* the persistent-vs-transient axis — see [memory](memory.md).
A persistent structure outlives the epoch that built it, and is rebuilt or refit only when its geometry changes.
A **transient (single-epoch) variant may come later** for structures rebuilt from scratch every frame, and it would be a property of the build call's result rather than a separate scope.
Build scratch is transient either way.

Ordering is inferred, never hand-synchronized.
A build declares [`accel_write`](../../src/shaped-graphics/barrier/resource_access.hh) on the `accel_build` stage over the structure it produces, and `accel_read` over each referenced BLAS.
A later trace declares `accel_read` on the `raytracing` stage, and the [barriers](barriers.md) system turns those into the right GPU barriers.

## Opacity micromaps

**A micromap is its own structure, as in both APIs**: `cmd.raytracing.build_micromap` builds one, and a `blas_triangles` names it with one index per triangle.
Each map subdivides its triangle into 4^level micro-triangles with a state each, and traversal decides an opaque or transparent micro-triangle with no candidate decision.
An index may instead be a `micromap_special_index`, a whole triangle's state, so a uniform triangle needs no map.
The BLAS holds every micromap it was built from, the edge a TLAS has with its BLASes.

- **The build is sized on the CPU**: both APIs take how many maps there are of each level and format, so `micromap_usage` must be known before a GPU pass writes the maps.
  A geometry names its own counts, which default to the micromap's.
- **The records are the APIs' own**: `micromap_descriptor` is the 8-byte record D3D12 and Vulkan both read, and states pack lowest bits first.
- **Strict where the feature is absent**: `build_micromap`, and a geometry naming a micromap, assert without `sg::feature::opacity_micromap`, so a caller checks before baking.
- **A ray query sees them through its flag**: SGL writes each `RayQuery` with it where the device has micromaps, which compiles it at Shader Model 6.9 (EMIT-184).
  vulkan would see them without it, but DXC takes a ray forced to 2 states (`ray_flags.force_omm_2_state`) only beside it.
  A ray-tracing pipeline is created with the micromap flag wherever micromaps are enabled, on either.

**vulkan** builds them through `VK_EXT_opacity_micromap`, enabled wherever the device grants it.
Vulkan builds micromaps in a pipeline stage sg's barrier vocabulary does not name, so each build is bracketed by two global barriers, one into that stage and one out of it.
sg's own tracking meanwhile records the buffers as acceleration-structure accesses.
**dx12** builds them as DXR 1.2's micromap arrays, where it runs the Agility SDK on a device of ray-tracing tier 1.2 and Shader Model 6.9, and reports the feature absent otherwise.
A geometry naming one becomes DXR's OMM triangles, linking its index buffer and the array's address.
metal and webgpu have no equivalent — [feat-sg-opacity-micromaps](../issues/feat-sg-opacity-micromaps.md).

## Partitioned TLAS

**A `partitioned_tlas` is a `tlas` of fixed capacity, updated in place** rather than rebuilt from an instance list.
`ctx.persistent.create_partitioned_tlas` allocates it empty from its capacities, recording nothing, and `cmd.raytracing.update` fills it.
- **An update writes, updates and clears slots, and translates partitions**, and only the partitions it touches are rebuilt.
  The global partition, `sg::global_partition`, treats each of its instances as a partition of its own, for what moves every frame.
- **The slots are the ownership edge**: a partitioned tlas holds every blas a slot names, and the list of an update keeps the ones it replaced until it has run.
- **It binds and traces as any tlas does**, `as_view()`, so a shader never knows which kind it traces.
- **Requires `sg::feature::partitioned_tlas`**, which vulkan grants with `VK_NV_partitioned_acceleration_structure`.
  dx12 grants it on the preview Agility SDK alone (`SC_AGILITY_PREVIEW`), where the device reports `ClustersAndPTLASSupported`, and binds it as a tlas.

**On vulkan it has no descriptor**: `VK_EXT_descriptor_buffer` can name only a `VkAccelerationStructureKHR`, which a partitioned tlas is not.
So every vulkan trace SGL writes takes its structure by device address, from the reserved `sg_acceleration_addresses`.
[reserved-bindings](../../../shaped-graphics-language/docs/reserved-bindings.md) is that block's contract.
That holds on both routes: SGL's own SPIR-V converts it, and the HLSL a DXC route compiles converts it through inline SPIR-V.
Only a package's own HLSL still reads the descriptor, where a partitioned tlas writes the null structure, which every ray misses.
[plans/clusters-and-partitioned-tlas.md](../plans/clusters-and-partitioned-tlas.md) is the design, and dx12's half.

### Updates a compute pass wrote

**`cmd.raytracing.update_indirect` builds from records a shader wrote**, so an update never visits the CPU.
- **The records are SGL's `accel` module's**, which a shader writes by field name: `accel.ptlas_write`, `accel.ptlas_update` and `accel.ptlas_translation`.
  Each backend lays them out natively, so nothing translates them before the build.
- **The shader writes the records and bumps a count; sg writes the operation array** and copies each count into it, so no shader writes an operation.
  `indirect_records` names a buffer of records, its `max_count`, and where the count is; a count past `max_count` is a caller bug.
- **A record names a BLAS by an address only a `blas_table` hands out**: `ctx.persistent.create_blas_table(blases)` holds them, and a shader binds `table->view()` as `buffer[accel.blas_ref]`.
  An update's `tables` are every table a record may name afterwards, which the structure holds in place of the earlier update's.
- **`accel.ptlas_write` is opaque on the host**: `sg::gpu_ptlas_write` sizes a buffer of them, since only a shader fills one.
  SGL's `@target_layout` is why: its fields stand in another order on dx12 and on vulkan.
- **No check follows the records**, so `hit_record_checks` stops checking a partitioned tlas from its first such update, and logs that once.

## Clustered geometry

**A cluster BLAS is built from clusters rather than from triangles**: up to 256 triangles over up to 256 vertices each.
- **`cmd.raytracing.build_clusters` builds a `cluster_set`**, immutable like a blas, cluster k at position k.
- **`build_cluster_blas(set, ranges)` builds one blas per range** of the set's clusters, and each holds its set.
  A range is contiguous because a cluster's address exists only on the GPU, in the set's address array, which the BLAS build reads in place.
- **A cluster BLAS is a blas like any other**: a tlas or a partitioned tlas places it, and a trace meets its triangles.
  A hit's `cluster_id` in SGL is the cluster's `cluster_id`, and its `primitive_index` the triangle's within the cluster.

**A `cluster_pool` is the GPU-written tier**: `cmd.raytracing.build_clusters_indirect` replaces its clusters in place, from `accel.clas_triangles` records a shader wrote.
- **A record names its buffers by address**, which only a `buffer_table` hands out: `ctx.persistent.create_buffer_table(buffers)`, bound as `buffer[accel.buffer_ref]`.
  The build names the tables, so their buffers are what it declares it reads.
- **A pool has a generation, which each build bumps**, and a cluster BLAS records the one it was built from.
  A stale one, `blas.is_stale()`, lists clusters that are gone: a tlas or a partitioned tlas build asserts on it.
  A structure built from it earlier must not be traced after the rebuild, and under `context::hit_record_checks` a dispatch or draw binding one asserts.
  A tlas updated from GPU-written records is not checked, since sg never sees its instances.
- **A rebuild waits behind every earlier trace, with one global barrier**, since a trace declares only its tlas's storage and reads the pool's clusters through it.
- **`accel.clas_triangles` differs in shape per backend**, so its host type `sg::gpu_clas_triangles` is opaque and sized for the larger record; each backend strides by its own.
- **Requires `sg::feature::clustered_geometry`**, which vulkan grants with `VK_NV_cluster_acceleration_structure`.
  dx12 grants it on the preview Agility SDK alone, with the partitioned tlas, and takes one index format per build.
  A dx12 shader reads -1 for `cluster_id` until the preview DXC is pinned, since Shader Model 6.10 has the name and slib masks the feature off.

## Invariants

Preserve these; the rest is tuning:

1. **Inputs stay within the DXR∩Vulkan common denominator** — `float3` vertices, 3×4 row-major transforms,
   24-bit `instance_id`/`hit_group_offset`, 8-bit `mask`.
   Widen only behind a capability query.
2. **A referenced BLAS is fully built before its TLAS**, and a `tlas` instance holds a `blas_handle` so the
   BLAS outlives every TLAS using it.
3. **Scratch is transient** (single-epoch), never retained.
   What the built structure itself *is* belongs to the backend — a buffer on DXR, an object of its own on Metal.
4. **Build/refit/trace ordering is inferred** from `accel_build`/`accel_write`/`accel_read` + the
   `raytracing` stage — resources are never hand-synchronized.

## dx12 implementation

- Sizing via `GetRaytracingAccelerationStructurePrebuildInfo` over the translated `D3D12_RAYTRACING_GEOMETRY_DESC` array (BLAS) or the packed `D3D12_RAYTRACING_INSTANCE_DESC` array (TLAS).
  The build is `ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure`.
- Instances pack into `D3D12_RAYTRACING_INSTANCE_DESC`: `InstanceID:24`, `InstanceMask`, `InstanceContributionToHitGroupIndex:24`, `Flags` (opaque override + cull), and the 3×4 `Transform`.
- Barriers come free, in [`dx12_barrier.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_barrier.cc).
  `accel_build` already maps to `BUILD_RAYTRACING_ACCELERATION_STRUCTURE` sync, `raytracing` to `RAYTRACING` sync, and `accel_read`/`accel_write` to the AS read/write barrier accesses.
  **Only the AS storage buffers are acceleration structures** — the build declares `accel_write` on its result and `accel_read` on each referenced BLAS.
  Build *scratch* is a plain UAV (`shader_write`) and the geometry/instance *inputs* are ordinary reads (`shader_read`).
  The enhanced-barrier AS access bits are illegal on non-AS buffers.
- **AS storage buffers are created in — and stay in — the `RAYTRACING_ACCELERATION_STRUCTURE` resource state**, with `ALLOW_UNORDERED_ACCESS`, rather than the usual `COMMON`.
  D3D12 forbids transitioning an AS resource, and both the enhanced AS barriers and `BuildRaytracingAccelerationStructure` require that state.
  This is the one buffer usage that overrides the backend's default COMMON creation, in [`dx12_buffer.cc`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_buffer.cc).
- **Under the Agility runtime they also carry `D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE`**, which a micromap array requires.
  The legacy creation calls drop that flag, so such a buffer is created through `CreateCommittedResource3` or `CreatePlacedResource2`, which take a layout rather than a state.

## metal implementation

- Sizing via `MTL::Device::accelerationStructureSizes`, which answers the result, build-scratch and **refit**-scratch sizes in one query.
  So `update_scratch_size_in_bytes()` is filled in from the start, long before anything refits.
- The structure is minted by `newAccelerationStructure(size)` and built on the **compute encoder**.
  MTL4 puts `buildAccelerationStructure` alongside dispatch and the copies that stand in for a blit encoder, so there is no separate acceleration-structure encoder to open.
- Residency is declared rather than inferred, like every other Metal resource.
  A structure outside the queue's residency set is simply not there when the build runs, with no validation message to say so.
- **MTL4 requires the instance descriptor Metal calls "indirect".**
  `setInstancedAccelerationStructures` — the side array the default and userID descriptors index into — exists only on the Metal 3 `MTL::InstanceAccelerationStructureDescriptor`.
  So a TLAS built through MTL4 uses `MTL::IndirectAccelerationStructureInstanceDescriptor`, whose `accelerationStructureID` names each BLAS by handle.
  Despite the name that is the *direct* analogue of DXR's by-address reference.
  Its fields — `options`, `mask`, `intersectionFunctionTableOffset`, `userID`, `transformationMatrix` — are exactly `sg::tlas_instance`'s.
  The name invites picking the wrong one.
- **Row-major 3×4 transforms are taken verbatim**, through `MTL::MatrixLayoutRowMajor` on the geometry descriptor — no transpose.
  The *instance* transform is the exception: `PackedFloat4x3` is four packed `float3` columns and its layout is fixed, so that one is transposed on the way in.
- The four `MTL::AccelerationStructureInstanceOptions` bits map 1:1 onto `instance_cull_mode` plus `opaque_override`.
- `metal-cpp`'s umbrella `Metal.hpp` does **not** include `MTL4AccelerationStructure.hpp`, and nothing else in the package does either.
  So those descriptors need that header named directly, unlike every other MTL4 type.
- **A TLAS also keeps each instance's `hit_group_offset` in a buffer of its own**, `metal_tlas::hit_group_offsets()`, one `u32` per instance.
  A pipeline's kernel reads it to find a closest hit's record, since Metal's intersection result names the instance and not its offset.
  [raytracing-pipeline](raytracing-pipeline.md#metal-maps-it-onto-a-compute-pipeline-because-a-raygen-shader-is-the-kernel) says how it is bound.
- **A structure that moves is a motion structure**, which a pipeline's trace reads natively and an inline query cannot read at all.
  Only a shader whose package takes `SOFTWARE_TRACE` is bound one: SGL writes the motion tags there alone, since they double a static scene's traversal.
  Its inline traces take the walk below, from builds with `accel_build_flag::software_trace`; its pipeline traces need no flag.

## dx12 and vulkan: the software walk, on request

Motion has no native trace on either, so a structure that moves is traced by the walk webgpu uses, on the three conditions below.
**Nothing picks this for you**: a device with or without hardware motion behaves the same, and the slowdown is one a build asked for.

- **The shader's package takes `SOFTWARE_TRACE`** in `sc_add_shader_package`, which writes its inline traces as the walk on every device.
  Its acceleration members come out with `sg::binding::traced_in_software`, so a layout holding one carries the pool and the roots.
  A ray-tracing pipeline's `trace_ray` has no such form and sees no motion.
- **The builds take `accel_build_flag::software_trace`**, a TLAS and every BLAS it places.
  A build still makes the native structure, whose moving parts stand at time 0, and the binding keeps its native descriptor.
- **The caller hands the geometry over on the CPU**, in the `cpu_*` spans of `blas_triangles`, `blas_pose` and `blas_aabbs`.
  They are read within the call, the description's offsets apply to them, and a missing one is an assert: sg reads no GPU buffer back for a build.

A binding not traced in software refuses a TLAS that moves when the group is made, naming `SOFTWARE_TRACE`.
A binding traced in software refuses a TLAS built without the flag.
Neither binding array nor staging group holds one traced in software yet, and a software geometry takes no micromap.

The pool here is **host-visible and written at record time**, so a build records no GPU work for it and needs no compiled kernel.
A region is new memory no recorded work reads, so nothing is ordered against it, and a shader reads it across the bus.
So the boxes are the CPU's, the layout the same as webgpu's: [raytracing-polyfill.md](../../../shaped-graphics-language/docs/raytracing-polyfill.md).

## webgpu implementation: a software polyfill

WebGPU has no acceleration structures, so sg builds its own and SGL's prelude traverses them; `ctx.implementation_of(sg::feature::ray_query)` is `emulated`.
The layout is a contract with SGL, and [raytracing-polyfill.md](../../../shaped-graphics-language/docs/raytracing-polyfill.md) is where it lives.

- **Every BLAS and TLAS is a region of one storage buffer per context**, the acceleration pool, suballocated in 16-byte units.
  A structure's handle is its offset there, so `size_in_bytes()` is its region's size.
- **The records are sorted by the Morton code of their centers, on the GPU, and the tree follows that order.**
  Leaves are consecutive runs of up to four records and each level pairs consecutive nodes, so the CPU writes the topology and compute kernels fill in the boxes.
  So a mesh traces about as fast in any triangle order; unsorted, a shuffled one traced up to 2500 times slower.
- **`fast_build` skips the sort**, which leaves the caller's order; every other flag is checked and changes nothing, with no refit or compaction behind it.
- **A pipeline layout binds at most 16 acceleration structures**, the size of the roots block the pool is traced from.
- **All structures together fit in 128 MiB**, WebGPU's default largest storage binding, since the pool is bound whole.
  A build past it throws `sg::allocation_exception`.

[backends/webgpu/readme.md](../../backends/webgpu/readme.md#ray-queries) covers the pool's growth, reuse and bindings.

The **vulkan** backend builds through `VK_KHR_acceleration_structure`, with `to_vk_buffer_usage` mapping both
`accel_structure_*` usages and the buffer device address they need.
Placed allocations, which a future transient variant would want, are not implemented there.

## What's implemented today vs deferred

**Today:** the single-shot build path, on all four backends, webgpu's as the polyfill above.
`sg::blas` / `sg::tlas` with `blas_handle` / `tlas_handle`, plus the input vocabulary: `blas_triangles`, `blas_aabbs`, `tlas_instance`, `accel_build_flags`, `instance_cull_mode`, `index_format`.
On the `cmd.raytracing` scope: `build_blas` for triangles and procedural AABBs, `build_tlas`, and `is_supported()`.
**dx12** is the reference realization — prebuild-sized result plus transient scratch, `BuildRaytracingAccelerationStructure`, gated on `D3D12_RAYTRACING_TIER` — and it runs on WARP.
**metal** builds on the compute encoder, as above, and reports `is_supported() == true` on every device above its Metal 4 floor.
**vulkan** builds through `VK_KHR_acceleration_structure`.
It reports `is_supported()` from `sg::feature::ray_query` and `sg::feature::raytracing_pipeline`, so a device without the extension answers false.

**The trace side is in.**
A `tlas` binds as a shader resource through the `acceleration_structure` binding type and view kind, for a ray query from any stage — on every backend.
The pipeline path is `raytracing_pipeline`, `raytracing_shader_table` and `cmd.raytracing.dispatch_rays`, on dx12 (WARP included), vulkan and metal.
See [raytracing-pipeline](raytracing-pipeline.md), which also says which feature each backend reports.

**Deferred** (see [epic-sg-acceleration-structure-gaps](../issues/epic-sg-acceleration-structure-gaps.md) and [epic-sg-raytracing-pipeline-gaps](../issues/epic-sg-raytracing-pipeline-gaps.md)):

- the **transient (single-epoch) variant**, and the **refit / update + compaction** runtime path;
- the shader table's own storage abstraction — it is backed by a plain readable buffer today, and [types.hh](../../src/shaped-graphics/types.hh) rules an SBT out of `buffer_usage` deliberately;
- **local root signatures** — a record stores only a 32-byte shader identifier today, under one global root signature.

## See also

- [types.hh](../../src/shaped-graphics/types.hh) — `buffer_usage::accel_structure_storage` / `accel_structure_build_input`.
- [resource_access.hh](../../src/shaped-graphics/barrier/resource_access.hh) — the `accel_*` access flags and stages.
- [barriers](barriers.md) — how AS build/trace accesses are tracked and ordered.
- [memory](memory.md) — the persistent-vs-transient lifetime axis the handle sits on.
- [bindings](bindings.md) — the `acceleration_structure` binding used to trace against a `tlas`.
- [cheat-sheet](../../cheat-sheet.md) — the AS API at a glance.
