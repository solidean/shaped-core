# shaped-graphics structure (sg::)

The living roadmap for shaped-graphics.
Section headers carry a status tag:

- **[done]** — implemented and tested
- **[in progress]** — partially implemented
- **[planned]** — not started
- **[stub]** — type/shape exists, body is `CC_UNREACHABLE("not implemented yet")`

Update the tags as the API lands.
This document is design intent, not a guarantee of final API.

## Goals

- A small, backend-agnostic graphics-API surface (`context`, `command_list`, GPU resources).
- The public `context` / `command_list` / `buffer` are abstract interfaces; concrete backends are
  independent static libraries that subclass them directly (no separate bridge layer), with
  backend-specific work duplicated rather than abstracted.
- Shared-immutable resources fronting GPU-resident memory; all host↔device transfer managed by
  sg (no host-visible resources exposed).

## Top-level structure

The tree is grouped by topic.
Only the cross-cutting vocabulary sits at the root — everything else lives in the folder that owns it, and is included by its full path (`#include <shaped-graphics/resource/texture.hh>`).
Where a folder is named after a type, that type's header repeats the folder name and doubles as the folder's umbrella.

```text
src/shaped-graphics/
  fwd.hh / all.hh / types.hh      [done]       fwd decls + *_handle typedefs; umbrella; the small vocabulary enums
  exceptions.hh                   [done]       typed sg exceptions (device_lost / allocation / pipeline_creation / swapchain_creation / binding_group)
  bytes_future.hh/.cc             [done]       bytes_future / data_future<T> / bytes_wait_gate — the result vocabulary every download shares

  transfer/                                    # the streaming tier: bulk transfers with a handle instead of automatic sync
    stream.hh/.cc                 [done]       the ctx.stream facade, scope validation, the ratio / aging knobs
    stream_handle.hh/.cc          [done]       stream_upload_handle / stream_download_handle — priority, progress, cancel, promote
    stream_source.hh/.cc          [done]       the lazy chunk sequence feeding an upload, plus the resident default
    stream_sink.hh                [done]       where a download's chunks go when a resident destination is not wanted
    impl/transfer_scheduler.hh/.cc [done]      job selection: PWM window sharing, priority, aging, family ordering (GPU-free)

  barrier/                                     # the access-tracking substrate; shared vocabulary, per-backend emission
    resource_access.hh            [done]       access_flag(s) / pipeline_stage_flag(s) / texture_layout / access_barrier
    resource_access_state.hh      [done]       the three-timeline declare/flush state machine
    subresource_state.hh          [done]       subresource_box + the covering partition over a texture's subresource domain
    access_inference.hh           [done]       which access/layout an op or a bound view implies
    command_list_slot.hh/.cc      [done]       dense per-command-list index into a resource's concurrent access-state slots

  binding/
    compiled_shader.hh            [in progress] bytecode blob + stage/format/entry + reflected bindings
                                                (context::accepted_shader_formats advertises what a backend takes;
                                                 producing one is shaped-shader-library's job — see docs/shaders.md)
                                                deferred: the reflected vertex-input / I-O signatures
    binding.hh/.cc                [done]        backend-agnostic reflection: binding + binding_type ((set,index); maps to view)
    sampler.hh                    [done]        sampler value type + filter/address/border/compare vocabulary; both backends real
    binding_group_layout.hh/.cc   [done]        abstract: one group's schema; dx12 = descriptor-table schema, vulkan = VkDescriptorSetLayout
    pipeline_layout.hh/.cc        [done]        abstract: ordered group layouts (bind slots); dx12 = root signature, vulkan = VkPipelineLayout
    binding_group.hh/.cc          [done]        abstract: group-layout instance bound to raw_views (named_view); a descriptor-heap range in both backends
    staging_binding_group.hh/.cc  [done]        abstract: mutable descriptor image that mints immutable groups (binding_slot -> slot table, shape-named setters, snapshot);
                                                dx12 = private non-shader-visible heap + CopyDescriptorsSimple, vulkan = a host descriptor image + one memcpy
    bindless_array.hh/.cc         [done]        non-owning view identity -> element index map over ONE array binding of a staging group
                                                (impl/slot_table.hh is the fixed-capacity table with the per-epoch stale sweep)

  command_list/
    command_list.hh/.cc           [in progress] abstract, single-use recorder; owns the seven scopes below and the backend seams they call
    upload.hh/.cc                 [done]        cmd.upload: inline host→device buffer + texture writes (both backends real)
    download.hh/.cc               [done]        cmd.download: inline readback of buffers + textures -> bytes_future (both backends real)
    copy.hh/.cc                   [in progress] cmd.copy: device→device buffer regions (both backends real); texture copies pending
    compute.hh/.cc                [done]        cmd.compute: bind_pipeline / bind_group / dispatch (both backends real)
    raster.hh/.cc                 [in progress] cmd.raster: rendering scope, bindings, viewport/scissor state, draws (dx12 real; vulkan stub)
    raytracing.hh/.cc             [in progress] cmd.raytracing: build_blas / build_tlas / dispatch_rays (dx12 real; vulkan stub)
    query.hh/.cc                  [in progress] cmd.query: record_gpu_timestamp / is_supported (dx12 real; vulkan stub)

  compute/
    compute_pipeline.hh/.cc       [done]        abstract: compute shader + pipeline layout; dx12 = PSO, vulkan = VkPipeline + VkPipelineCache

  context/
    context.hh/.cc                [in progress] abstract; infallible create_command_list over pure-virtual try_create_*; sticky device-loss status; every create funneled through a scope
    persistent.hh/.cc             [done]        ctx.persistent: the persistent-lifetime resource factory
    transient.hh/.cc              [in progress] ctx.transient: per-epoch bump allocator over one owned memory_heap (buffers only; textures fall back to dedicated)
    upload.hh/.cc                 [in progress] ctx.upload: async bulk streaming on the dedicated copy queue (dx12 real; vulkan stub)
    download.hh/.cc               [in progress] ctx.download: async bulk readback on the copy queue -> bytes_future (dx12 real; vulkan stub)
    uncached.hh/.cc               [in progress] ctx.uncached: the raw, non-memoized layout / pipeline factory
    cached.hh/.cc                 [in progress] ctx.cached: get-or-create over pipeline_cache (layouts sync, pipelines async); raster pipelines still missing
    pipeline_cache.hh/.cc         [in progress] content-addressed tiered cache for group layouts, pipeline layouts, compute + raytracing pipelines

  memory/
    allocation_info.hh            [done]        value type: placement handle (heap/offset/size + scope); null heap = dedicated
    memory_heap.hh/.cc            [in progress] abstract heap + memory_requirements; both backends place buffers, textures still dedicated-only

  present/
    swapchain.hh/.cc              [in progress] swapchain_description + swapchain (acquire_backbuffer / present / resize / HDR flag); dx12 real (vulkan stub)

  query/
    gpu_timestamp.hh/.cc          [done]        pollable tick result of cmd.query.record_gpu_timestamp

  raster/
    raster_pipeline.hh/.cc        [in progress] abstract graphics PSO + raster_pipeline_description; dx12 real (vulkan stub)
    primitive_topology.hh         [done]        topology + topology_type
    rasterization_state.hh        [done]        fill/cull/front-face + depth bias
    blend_state.hh                [done]        blend factors/ops, per-target blend + write mask
    depth_stencil_state.hh        [done]        depth test/write, stencil faces + ops (reuses compare_op)
    vertex_input.hh               [in progress] vertex_input_layout / slots / attributes; attributes are still HLSL-semantic-keyed

  raytracing/
    acceleration_structure.hh/.cc [in progress] blas / tlas + their build inputs; dx12 real (vulkan stub)
    raytracing_pipeline.hh/.cc    [in progress] DXR state object + the shader-handle registration phase; dx12 real (vulkan stub)
    raytracing_shader_table.hh/.cc [in progress] shader-table description + abstract table; dx12 real (vulkan stub)

  resource/
    pixel_format.hh               [done]        restrictive texel-format enum + helpers (depth/compressed/block-size)
    subresource.hh                [done]        texture_aspect / subresource_extent / _index / _range — pure addressing vocabulary
    raw_buffer.hh/.cc             [in progress] abstract; protected shape (size/usage); as_* view factories
    buffer.hh                     [done]        buffer<T> typed wrapper over raw_buffer_handle — element type pinned once at creation
    raw_texture.hh/.cc            [in progress] abstract; texture_description + protected shape
    texture.hh                    [in progress] texture<Traits> typed wrapper (concept-gated getters) + shape typedefs
    texture_traits.hh             [done]        compile-time shape (dimension / array / cube / multisampled) + view-factory parameter bags
    texture_descriptions.hh       [done]        shape-specific description structs feeding the typed create_texture_* calls
    texture_region.hh             [done]        a texel box within one subresource, for host↔device copies
    views.hh                      [in progress] typed buffer views (uniform/readonly/readwrite<T>, byte=raw) + the erased raw_view;
                                                texture SRV/UAV + render_target / depth_stencil views; texel buffer views deferred
    vertex_buffer_view.hh         [done]        buffer + byte range + stride
    index_buffer_view.hh          [done]        buffer + index_format + byte range
    impl/                                       internal (sg::impl), outside the public header set

  routine/
    render_routine_base.hh        [in progress] abstract base of a registered routine (init/evict hooks)
    render_routine.hh/.cc         [in progress] render_routine<Derived> CRTP: 3-phase, hot-reload-aware acquire / prewarm / evict
    routine_registry.hh/.cc       [in progress] the per-context ctx.routines type-keyed registry
    reload_generation.hh/.cc      [done]        process-global monotonic counter driving hot-reload invalidation

backends/                                       # each subclasses the abstract sg types directly
  dx12/                           [in progress] sg::backend::dx12 + sg::create_dx12_context (Windows): real device/cmd-list/buffer/texture
    tests/                                      own *-test binary for dx12-specific tests (WARP + hardware)
  vulkan/                         [in progress] sg::backend::vulkan + sg::create_vulkan_context (native desktop): device + resource creation; recording is stubbed
  metal/                          [planned]     tier 2
  webgpu/                         [planned]     tier 2
  opengl/                         [planned]     legacy compat
  webgl/                          [planned]     legacy compat
```

## Backend tiers

- **Tier 1 (now):** dx12, vulkan.
  dx12 is real across the surface.
  vulkan brings up the device, its single queue and the epochs, and creates command lists, buffers and textures.
  Every other `try_create_*`, both async transfer scopes, and all recording are still stubs.
- **Tier 2 (soon):** metal, webgpu.
- **Legacy compat (planned):** opengl, webgl.

A backend is built only where its platform allows it — the gates are platform-only (dx12 → Windows, vulkan → native desktop).
dx12 links the Windows-SDK D3D12 libs (`d3d12 dxgi dxguid`), always present on the Windows path.
vulkan gates on `find_package(Vulkan)` and links `Vulkan::Vulkan`, so it builds wherever a Vulkan SDK is installed.

Error handling follows the repo policy in [docs/error-handling.md](../../../../docs/error-handling.md).
A resource create offers a throwing default: `create_raw_buffer` returns the handle and raises a typed `sg::exception` (see `exceptions.hh`).
Beside it sits a fallible `try_create_*` returning `cc::result` — the only one backends actually implement.
`create_command_list()` is infallible, throwing only on device loss; `create_<backend>_context` returns `cc::result`, since bringing a device up is an environment failure.
Programmer misuse asserts rather than returning an error — `size < 0`, a missing usage, a transient resource used past its epoch.
Device loss is a sticky global status (`ctx.is_device_lost()`) surfaced by a throw at submit / advance / fence waits, deliberately kept off the `try_*` channel.

## Context creation & backend decoupling

sg never depends on a backend — the arrow is backends → sg, so there is no `sg::create_context` in the core.
Each backend library exposes an `sg::create_<backend>_context(config)` factory in the `sg` namespace, with its own config type.
Rules and rationale: [coding-guidelines](coding-guidelines.md).

## Resource & transfer model

Resources (`raw_buffer`, `raw_texture`) are **shared-immutable**: a fixed shape, span-like over mutable GPU memory, held via `*_handle`, and legitimately **empty** at size 0.
There are **no host-visible resources** — host↔device transfer is a globally shared resource sg manages, driven through command lists.
Rules and rationale: [coding-guidelines](coding-guidelines.md).

Every create is reached through a **scope** on the context rather than the context itself: `ctx.persistent.create_raw_buffer(...)`.
A scope is a thin facade holding a back-reference to its context, and the `create_*` virtual stays on `context` for backends to implement.
The scope — a friend — funnels through that virtual, tagging the request with its lifetime.
Which scope creates what, and how to choose between them: [concepts/context.md](concepts/context.md).

## Ownership & lifetime

- **Resources are shared** (`raw_buffer_handle` = `shared_ptr`); a **command list is move-only** (`std::unique_ptr<command_list>`, no handle typedef) — record once, submit once, passed by reference.
- **Backend-typed create methods** (`create_dx12_buffer` → `dx12_buffer_handle`, …) are the real implementations, and the abstract `sg::context` virtuals are thin forwarders over them.
- A **context must outlive** every object it creates, and is **shut down before destruction** (virtual `shutdown()`, auto-run by the backend destructor).

Rationale for each: [coding-guidelines](coding-guidelines.md).

## Memory placement

Every resource's backing memory is either **dedicated** (self-allocating) or **placed** into a shared `memory_heap`, and the `allocation_info` passed to each `create_*` names which.
`memory_heap` is an immutable factory for `allocation_info` — allocation tracking lives in the caller's own allocator on top.
Lifetime modes and the placed-vs-dedicated system: [concepts/memory.md](concepts/memory.md).

`context` exposes `try_create_memory_heap`, and placement is live for **dx12 buffers** — that is what `ctx.transient`'s bump allocator runs on.
Still dedicated-only: **dx12 textures** and **all vulkan resources**, both of which assert on a placed `allocation_info`.

## Planned surface (beyond the current stubs)

```text
buffer transfer      [in progress]  command_list inline upload / download / copy (dx12 real, vulkan stub)
texture transfer     [in progress]  inline + async host↔device texture copies, region- and subresource-scoped
                                  (dx12 real, vulkan stub); device→device texture copy is the gap
barriers             [in progress]  inferred access + state tracking + concurrent-list slot model; dx12 enhanced
                                  barriers real for buffers + textures (subresource-range layout transitions,
                                  entry-layout revert), driven by texture transfer and the rendering scope;
                                  vulkan pending
views                [in progress]  strongly-typed resource views; buffer + texture (SRV/UAV) views done (dx12 bindable
                                  in compute); render_target/depth_stencil views done and consumed by the rendering
                                  scope; texel buffers deferred
bindings             [in progress]  compiled_shader + binding vocab; binding_group_layout / pipeline_layout / group + compute_pipeline (dx12 real, vulkan stub).
                                  Bounded array bindings, staging_binding_group (the mutable builder behind a bindless table) and bindless_array over one of its
                                  array bindings are in; unbounded arrays are rejected at layout creation (WebGPU has none — see concepts/bindings.md)
texture              [in progress]  raw_texture + texture<Traits> + pixel_format; creation, dx12 layout barriers,
                                  SRV/UAV + RTV/DSV views and host↔device copies done; device→device copies remain
pipeline             [in progress]  compute + raster pipelines and the bind path (dx12 real, vulkan stub); shaders are
                                  compiled by shaped-shader-compiler-dxc. Raster PSO caching is the remaining gap
sampler              [in progress]  sampler + static/dynamic samplers; dx12 real (root-sig static samplers
                                  + a separate sampler descriptor heap for dynamic ones); vulkan pending
accel structures     [in progress]  ray-tracing blas/tlas: recorded build on cmd.raytracing (build_blas for
                                  triangles + procedural AABBs, build_tlas, is_supported), result sized from a
                                  prebuild query with transient scratch, persistent handles across epochs;
                                  dx12 real (WARP), vulkan stub. Deferred: transient variant, refit/update, compaction
raytracing pipeline  [in progress]  raytracing_pipeline + shader table + cmd.raytracing.dispatch_rays, and the
                                  acceleration_structure binding (inline RayQuery); dx12 real (WARP), vulkan stub.
                                  Deferred: local root signatures, a dedicated shader-table buffer usage, a cached blob
gpu queries          [in progress]  cmd.query.record_gpu_timestamp -> gpu_timestamp; pooled query heaps leased
                                  per list, one batched inline readback per heap at submit; dx12 real (WARP),
                                  vulkan stub. Deferred: occlusion + pipeline-statistics queries
swapchain / surface  [in progress]  ctx.create_swapchain -> sg::swapchain (acquire_backbuffer -> render_target_view;
                                  present via ctx.submit_command_list_and_present; once-per-epoch auto-resize;
                                  vsync/immediate; HDR flag); dx12 real (IDXGISwapChain3 flip model, back buffers as
                                  borrowed dx12_texture on the render-pass path); vulkan stub. Deferred: deeper HDR,
                                  multi-window, exclusive fullscreen
epochs / submission  [in progress]  epoch counter + direct-queue epoch/submission timelines, advance/retire,
                                  deferred deletion + finalizers, allocator/pool recycling (dx12 + vulkan real),
transient resources  [in progress]  ctx.transient's per-epoch bump allocator over a memory_heap + the transient
                                  descriptor ring (dx12 real); transient textures are dedicated, auto-expired next epoch
                                  descriptor ring (dx12 real); transient textures pending
```

The **epoch system** underpins safe resource reclamation and command-allocator recycling.
Only the vocabulary is shared — `sg::epoch` / `sg::submission_token` and the `sg::context` contract — and each backend realizes it for itself.
See [concepts/epochs.md](concepts/epochs.md).

## Initial implementation order

```text
1. core types + backend bridge stubs + dx12/vulkan stubs   [in progress]  (this bootstrap)
2. command_list buffer inline upload / download / copy     [in progress]  dx12 real; vulkan pending
3. real dx12 + vulkan backends for (2) (+ SDK detection)   [in progress]  dx12 done; vulkan is a TODO stub
4. textures + views                                        [in progress]  resource, creation, views and host↔device copies done (dx12 real, vulkan minimal); texel buffer views remain
5. pipelines + shaders                                     [in progress]  compute + raster bind paths dx12-real, DXC compiler in place (vulkan pending)
6. presentation (swapchain/surface) + submission/sync      [in progress]  dx12 swapchain real (WARP-tested); vulkan pending
7. tier 2 backends (metal, webgpu)                         [planned]
8. legacy backends (opengl, webgl)                         [planned]
```
