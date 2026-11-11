# Plan: clustered geometry and partitioned TLAS

Status: **phase 1 built**, which the [phases](#phases) describe.
The [decisions](#decisions) stand as proposals except D5's, and [open questions](#open-questions) lists what the specs leave unsettled.
D3D12 and Vulkan each gained two acceleration-structure kinds that change how a large scene is kept current: clusters and partitioned top-level structures.
This plan is how sg exposes them, on dx12 and vulkan only.
Background: [acceleration structures](../concepts/acceleration-structures.md) for the model this plan departs from, and [barriers](../concepts/barriers.md) for how ordering is inferred.

## What the two features are

**A partitioned TLAS (PTLAS)** is a top-level structure with fixed instance and partition capacities, updated in place by ops rather than rebuilt from an instance list.
- An op writes an instance, updates one's BLAS and hit-group offset only, or translates a whole partition.
- Only the partitions an op touches are rebuilt, so a scene of a million instances with a hundred moving costs a hundred.
- A global partition holds instances that move every frame, each one treated as a partition of its own.
- It traces exactly like a TLAS, and an instance may place any BLAS, a cluster BLAS included.

**Clustered geometry** builds a BLAS from small pre-built pieces rather than from triangles.
- A CLAS (cluster-level structure) is up to 256 triangles over up to 256 vertices, built on the GPU from arguments the GPU wrote.
- A cluster BLAS is a list of CLAS addresses, so a LOD change swaps clusters and rebuilds only the cheap top of the BLAS.
- A cluster template holds topology without positions, and instantiating it with positions makes a CLAS faster than a build.
- Shaders see `ClusterID()`, a 32-bit value the CLAS was given, and `PrimitiveIndex()` is local to the cluster.

**Both are driven indirectly**: every argument, count and destination address may come from a GPU buffer a compute pass wrote.
So a compute pass can choose LODs and cull instances without a CPU round trip, which the current sg model cannot express.

## Where it runs

| backend | PTLAS | clusters | how |
|---|---|---|---|
| dx12 | preview | preview | Agility SDK 1.722-preview, `ExecuteIndirectRTASOperations`; NVIDIA RTX, AMD RX 9000, Intel Arc B and Core Ultra 200V, WARP |
| vulkan | NVIDIA | NVIDIA | `VK_NV_partitioned_acceleration_structure`, `VK_NV_cluster_acceleration_structure` |
| metal | no | no | no equivalent in Metal 4 |
| webgpu | no | no | the software walk has no indirect build, and nothing here is worth emulating |

The two Vulkan extensions are independent, and D3D12 reports both under one cap bit, `ClustersAndPTLASSupported`.
**So sg reports two features**, `sg::feature::partitioned_tlas` and `sg::feature::clustered_geometry`, and dx12 grants both or neither.
A PTLAS over ordinary BLASes is useful on its own, which is why the split is worth having.

## Why it does not extend `build_blas` / `build_tlas`

Today a `tlas` knows its instances on the CPU, and four things rest on that:

1. **Ownership**: `tlas_instance::blas` is a `blas_handle`, and holding it keeps the BLAS alive.
2. **`context::hit_record_checks`** walks each instance's BLAS, offset and mask before a trace.
3. **The software walk** rebuilds the structure on the CPU from the description's `cpu_*` spans.
4. **Metal's `hit_record_stride`** is baked into each BLAS.

An indirect op breaks the first three.
The GPU writes which BLAS an instance places, and which CLAS a cluster BLAS holds, so the CPU never sees the edge.
The fourth does not arise, since metal has neither feature.

So ownership moves up a level, from what an instance names to what a structure is *allowed* to name.
That is the one idea the decisions below elaborate.

## Decisions

### D1. Capacity objects are allocated by `ctx`, and operations are recorded on `cmd.raytracing`

`build_tlas` sizes, allocates and builds in one recorded step, because its size comes from the build's own inputs.
A PTLAS or a CLAS pool is sized from declared capacities and limits instead, through a CPU-side prebuild query that records no work.
So allocation returns to the rule every other sg resource follows:

- `ctx.create_partitioned_tlas({.instance_capacity, .partition_count, .max_instances_per_partition, .max_global_instances, .flags})`.
- `ctx.create_cluster_pool({.limits, .flags})`, where `limits` is the D3D12 / Vulkan intersection: max clusters, triangles and vertices per cluster and in total, max geometry index, vertex format.
- `cmd.raytracing.update(ptlas, ...)` and `cmd.raytracing.build_clusters(pool, ...)` record the operations, with transient scratch as today.

A structure made this way is persistent and updated in place for its whole life.
Changing a PTLAS's capacities is a new PTLAS, not an op, since both APIs price it like one.

### D2. A PTLAS is a `tlas`

`sg::partitioned_tlas` derives from `sg::tlas`, so `as_view()`, every binding and both trace paths take it unchanged.
D3D12 says outright that a PTLAS traces exactly like a TLAS.
Its `accel_read` / `accel_write` accesses are the structure's own, so an in-place update after a trace orders itself through the existing barrier tracking.

**Vulkan is the exception**: a PTLAS binds through its own descriptor type, `VK_DESCRIPTOR_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_NV`.
A descriptor set layout is fixed before anything is bound to it, so the layout has to know a binding takes a PTLAS.
See [open question Q1](#open-questions).

### D3. Two tiers per operation: described and GPU-written

Both APIs read every operation's arguments from GPU memory, so the difference between the tiers is only who writes them.

**Described**: the caller passes C++ values naming handles, and sg writes the native records into transient upload memory.
- A PTLAS op names a `blas_handle`, the same fields as `tlas_instance`, an instance index and a partition.
- A cluster build names buffers and offsets, as `blas_triangles` does today.
- sg validates each record, keeps the exact ownership edges, and mirrors every instance so `hit_record_checks` keeps working.

**GPU-written**: the caller passes a buffer of records a compute pass wrote, the count's buffer and offset, and a maximum count.
- sg declares the accesses from an **input set** the call names: every buffer and structure the records may reference.
  It is the same contract as `declare_array_buffer_access`, and an access the set omits is a caller bug sg cannot see.
- `hit_record_checks` cannot follow the records, so a PTLAS written this way stops being checked.
  It logs that once, rather than reporting a check it did not run.

A caller starts on the described tier and moves a hot path to the GPU-written one, on the same structure.

### D4. Ownership lives in tables the GPU indexes, never in raw addresses

sg exposes no GPU address today, by design: Metal has none, and an address is invisible to barrier tracking.
The GPU-written tier needs addresses in its records, so sg hands them out only through tables it owns:

- **A PTLAS's BLAS table** is a set of `blas_handle`s the caller registers, and the PTLAS holds every one alive.
  sg keeps their addresses in a buffer a shader reads, so a compute pass writes `table[i]` into a record, never a number it made up.
  Unregistering a BLAS is deferred until no recorded update can still name it.
- **A cluster pool's address array** is written by the build itself, in implicit-destination mode, one address per CLAS.
  A cluster BLAS holds its pool, which is the edge a TLAS has with its BLASes today.
- **An input set's buffer table** gives each named buffer's base address, for the vertex and index addresses in a cluster record.

**A pool has a generation**, bumped by every build into it, because rebuilding or moving a CLAS invalidates each cluster BLAS built from it.
A cluster BLAS records the generation it was built against.
The described tier asserts on a stale one, and the GPU-written tier cannot tell.

### D5. The records have one layout per backend, and SGL writes them

The two APIs' records hold the same fields in different orders and widths.
- D3D12's write-instance record places the BLAS address before the instance index and the AABB last; Vulkan's places the AABB second and the address last.
- Even the flag values differ: an explicit AABB is `0x40` on D3D12 and `0x10` on Vulkan.
- A Vulkan CLAS record packs its counts into 9- and 4-bit fields where D3D12 uses `UINT16`s.

So "one layout everywhere", which the micromap descriptor could choose, is not available here.
Two ways remain, and the first is decided:

- **SGL emits the records** (decided).
  A builtin module, say `use accel`, declares `accel.ptlas_write_instance`, `accel.clas_triangles` and the rest as SGL structs whose layout is the target's.
  A compute shader fills them by field name, and each backend's emission lays them out natively, so nothing translates at run time.
  sg's C++ side gets the same layouts, for the described tier, from one generated header.
- **sg defines its own layout and transcodes it** in a compute pass before each operation.
  It works from HLSL and WGSL alike, at a dispatch and a buffer per operation, and sg would carry a kernel of its own.
  Rejected: the GPU-written tier exists for hot paths, and a conversion pass per operation is the cost it was meant to avoid.

### D6. Barriers learn one new access

Every new buffer is an access sg already names, except the argument records:
- Results, sources and destinations are `accel_write` / `accel_read`, on `accel_build`.
- Scratch is `shader_write`, and the input set's buffers are `shader_read`.
- The records and count are read by the build: a shader read on D3D12, not `INDIRECT_ARGUMENT`, and `INDIRECT_COMMAND_READ` at the build stage on Vulkan.

That last pair is neither `indirect_read` nor `shader_read` on both backends, so it gets a flag of its own, `accel_build_args`.
Mapping it to `indirect_read` would emit an illegal D3D12 barrier, and to `shader_read` a missing Vulkan one.

Operations in one native call may run in any order, so dependent stages are separate sg calls.
A CLAS build, then the cluster BLAS, then the PTLAS update is three calls, ordered by the accesses each declares.

### D7. SGL gains the cluster builtins, and pipelines opt in where the feature is enabled

- `cluster_id` in any-hit and closest-hit shaders, and on a ray query's candidate and committed hit, with the invalid value `0xffffffff`.
  It needs Shader Model 6.10 on dx12, and `ClusterIDNV` on vulkan; metal and wgsl answer `unsupported-yet`.
- A triangle's object-space positions, `TriangleObjectPositions()` and its ray-query forms, are useful beyond clusters.
  Vulkan has them as `VK_KHR_ray_tracing_position_fetch`, so they are a third feature, with their own plan.
- D3D12 traces a cluster only from a pipeline created with `ALLOW_CLUSTERED_GEOMETRY`, or a ray query carrying its flag.
  Vulkan wants `VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV`.
  The proposal follows the micromap precedent: set wherever the feature is enabled, so no pipeline has to say it may meet a cluster.
  See [Q3](#open-questions) for the cost of doing so.

### D8. The preview runtime is a channel of its own

Retail `1.620.1` stays the default Agility pin, so a default build and test run needs no Developer Mode.
- `fetch-agility.py --preview` installs `1.722.1-preview` into `extern/agility-preview`, and `SG_HAS_AGILITY_PREVIEW` (1/0) says which one a build took.
  Its terms forbid distribution and expire, so it is a development pin only: [platforms](../../../../../docs/platforms.md#preview-agility-sdk-sc_agility_preview) has the switch.
- `extern/dxc` needs a matching preview pin, `1.10.2605.37-preview`, since `1.9.2602.24` stops at Shader Model 6.9.
  Only `cluster_id` needs it, so it waits for phase 4, and its terms carry the same restrictions.
- `Microsoft.Direct3D.WARP 1.65535.22-preview` supports both features, in `extern/warp-preview`, which would make dx12 tests runnable on any machine.
- The dx12 backend opts in through `ID3D12DeviceFactory::EnableExperimentalFeatures` on its own factory, with `D3D12ExperimentalShaderModels` and `D3D12RaytracingPreview`.
  It already loads through `CreateDeviceFactory`, so the opt-in touches no other D3D12 user in the process.
  Without Developer Mode the preview runtime does not load at all, and dx12 falls back to the system runtime with a warning.
- Every preview identifier lives in one dx12 file behind `SG_HAS_AGILITY_PREVIEW`, because the spec is at v0.38 and still renames fields.

## Phases

1. **Plumbing** (built): the preview channel for Agility and WARP, `--agility-preview`, the opt-in, and dx12's `supports_clusters_and_ptlas()`.
   No sg feature reports either yet, since a reported feature promises a code path that does not exist until phase 2.
2. **PTLAS, described tier**, over ordinary BLASes: `sg::feature::partitioned_tlas`, and Vulkan's extension enabled where granted.
   Then `ctx.create_partitioned_tlas`, `cmd.raytracing.update` with the three ops, and `hit_record_checks` on the mirror.
   This alone retires a full TLAS rebuild per edit, the cost shaped-viewer pays on a large scene today.
3. **PTLAS, GPU-written tier**: the BLAS table, the input set, `accel_build_args`, and the record layouts of D5.
4. **Clusters**: `sg::feature::clustered_geometry`, the pool, CLAS builds in implicit-destination mode, cluster BLAS, the generation check, and SGL's `cluster_id` with the preview DXC.
5. **Templates and moves**, once a consumer needs them: instantiation, and moves as the only way to compact cluster memory.

Each phase lands tested on dx12 (WARP preview) and on vulkan where an NVIDIA device is present.

## Open questions

- **Q1: Can a Vulkan PTLAS bind as an ordinary acceleration structure?**
  The refpages show only the partitioned descriptor type, and do not say whether a `VkAccelerationStructureKHR` may wrap one.
  If not, a binding declares it takes a PTLAS, and SGL learns which bindings may.
- **Q2: D5's record layouts** — settled: SGL emits them, so sg's GPU-written tier is reached from SGL.
- **Q3: What does `ALLOW_CLUSTERED_GEOMETRY` cost a pipeline that never meets a cluster?**
  Neither spec says; it decides whether D7 sets it everywhere or a pipeline asks for it.
- **Q4: Does a PTLAS see opacity micromaps?**
  The instance flags name them, but the PTLAS description has no `ALLOW_OMM` of its own while every other operation needs it.
- **Q5: Can one write and one update op in a single call name the same instance?**
  D3D12's wording allows either reading; sg treats it as forbidden until the spec says otherwise.
- **Q6: What does a cluster BLAS report as its geometry count?**
  Geometry indices are per triangle, so the count a hit record needs is the declared `max_geometry_index + 1`.

## Sources

- D3D12: [the announcement](https://devblogs.microsoft.com/directx/clas-ptlas-preview/).
- D3D12: [DXR functional spec, part 2](https://github.com/microsoft/DirectX-Specs/blob/master/d3d/Raytracing2.md), v0.38.
- Vulkan: [VK_NV_cluster_acceleration_structure](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_cluster_acceleration_structure.html).
- Vulkan: [VK_NV_partitioned_acceleration_structure](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_partitioned_acceleration_structure.html).
- Samples: `D3D12RaytracingClusteredGeometry` and `D3D12RaytracingPartitionedTLAS` in [DirectX-Graphics-Samples](https://github.com/microsoft/DirectX-Graphics-Samples).
  The latter was ported from NVIDIA's `vk_partitioned_tlas`.
