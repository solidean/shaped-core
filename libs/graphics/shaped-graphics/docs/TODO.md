# shaped-graphics TODO

Running list of known follow-ups. Bigger design intent lives in
[structure.md](structure.md).

- **Buffer transfer — remaining:** inline buffer upload / download / copy is in (sg `command_list` API +
  `bytes_future`, real dx12 over UPLOAD / READBACK ring buffers, a `cc::threaded_actor` for deferred
  readback copies). Still open: the **vulkan** implementation (currently a `CC_UNREACHABLE` stub);
  **texture** upload/download (the `dx12_resource_upload/_download` helpers are shaped for it, concrete
  texture impls are TODO); and download robustness — the readback ring's free watermark advances in
  submit order, which only matches allocation order for single-threaded recording (needs per-region
  tracking / the split GPU-CPU watermarks), plus fallback staging when a single list's inline transfers
  exceed the ring capacity (currently asserts).
- **Barriers + access tracking — remaining:** the access-tracking system is in for **buffers** (inferred
  access, the three-timeline `resource_access_state`, the command-list slot model with revert/promote, and
  dx12 enhanced-barrier emission — see [concepts/barriers.md](concepts/barriers.md)). Uploading then
  downloading / self-copying the *same* buffer now works in one command list. **Textures** are now tracked
  too: each `dx12_texture` owns a per-command-list covering partition and emits subresource-range
  `D3D12_TEXTURE_BARRIER` layout transitions (with entry-layout revert on a non-final submit) — dx12-owned
  tracking + emission, since barrier models differ across backends. A bound texture in a compute dispatch
  now *drives* it: SRV/UAV texture views (`texture<Traits>::as_*_view`) transition to `shader_read` /
  `storage` at dispatch via `shader_layout_of`. Still open: **texture copy / upload / download** ops (so a
  sampled texture can be populated; the barrier system will order those too); render-target/depth-stencil
  views; **vulkan** barrier emission (lands with its compute/transfer
  milestone; it reuses the shared vocabulary + state machine); `declare_array_access` **full wiring** (API + validation are in, but applying it needs
  an array binding path + a binding-name→resource reflection map); migrating `access_flags` /
  `pipeline_stage_flags` to `cc::flags` when that lands; a per-draw/dispatch **escape hatch** that disables
  automatic transitions for callers that know their resources are already in the right layout; and folding
  the redundant `_open_command_lists` epoch-advance counter into the slot allocator's live count.
- **Raster pipeline + draws — deferred layers:** the graphics path is in (`sg::raster_pipeline` +
  `raster_pipeline_description` with its fixed-function state vocabulary — `primitive_topology`,
  `rasterization_state`, `blend_state`, `depth_stencil_state` reusing `compare_op`, `vertex_input_layout`
  with a type-driven `create<Vs...>()`; `ctx.uncached.create_raster_pipeline`; and draw recording on
  `cmd.raster` / `cmd.raster.manual` — `bind_pipeline` / `bind_group` / `bind_vertex_buffers` /
  `bind_index_buffer` / `set_viewport` / `set_scissor` / `set_stencil_reference` / `set_blend_constants` /
  `set_inline_constants` / `draw` / `draw_indexed`; dx12 real on WARP, vulkan stubbed). See
  [concepts/raster-pipeline.md](concepts/raster-pipeline.md). Still open: **PSO caching**
  (`ctx.cached.acquire_raster_pipeline` + `pipeline_cache` description hashing + `async_raster_pipeline` —
  the compute/RT parity piece); **indirect draws** (`draw_indirect` / count buffers); **dynamic primitive
  topology** and **dynamic depth bias** (both baked into the PSO for now); **mesh / task** stages; and the
  **vulkan** implementation (`VkPipeline` + dynamic-rendering formats + the `vkCmdDraw*` seams — currently
  `CC_UNREACHABLE`). **Geometry** and **tessellation** (hull/domain) stages are now in for dx12:
  `raster_pipeline_description::geometry_shader` / `tessellation_control_shader` /
  `tessellation_evaluation_shader`, the `patch_list` topology + `patch_control_points`, and the DXC gs/hs/ds
  profiles.
- **Acceleration structures — deferred layers:** the single-shot build path is in (`sg::blas`/`sg::tlas`,
  the `cmd.raytracing` scope with `build_blas` for triangles + procedural AABBs, `build_tlas`, and
  `is_supported()`; dx12 real on WARP, vulkan stubbed). The abstract types already carry the stats a refit
  needs (build + update scratch sizes, flags, the storage handle). Still open: the **transient (single-epoch)
  AS variant** (per-frame rebuilds) — a property of the build call's result, not a new scope; **refit /
  update** (reuses topology, needs `allow_update` at build and `PERFORM_UPDATE` + the source AS at build
  time) and **compaction** (BLAS `allow_compaction` → query compacted size → copy into a smaller buffer);
  the **vulkan** implementation (`to_vk_buffer_usage` must map the `accel_structure_*` usages + add the
  buffer device address; then the real `VkAccelerationStructureKHR` build path — flip the
  `nx::config::disabled`/`register_backend` toggle in `tests/backends/vulkan-entry.cc` once it lands). The
  **trace side** is implemented for dx12 — the `acceleration_structure` binding (inline `RayQuery`) plus the
  `raytracing_pipeline` / `raytracing_shader_table` / `cmd.raytracing.dispatch_rays` DXR path (see
  [concepts/raytracing-pipeline.md](concepts/raytracing-pipeline.md)); still open there are **local root
  signatures**, a **dedicated shader-table buffer usage** (`types.hh` reserves `shader_binding_table`), a
  **state-object cached blob**, and the **vulkan** trace implementation.
- **`cc::shared_ptr`:** the `*_handle` typedefs use `std::shared_ptr` as a placeholder. Surface a
  `cc::shared_ptr` in clean-core and switch handles to it (keeps sg off `std::`). See the
  [coding-guidelines](coding-guidelines.md) note.
- **`cc::flags`:** `buffer_usage` uses a hand-rolled `enum class` + bitwise operators; migrate to
  `cc::flags` once that clean-core type is implemented.
- **Views — deferred layers:** buffer views (`uniform`/`readonly`/`readwrite`, `byte` = raw) + the
  erased `raw_view` are in, as are texture SRV/UAV views and `render_target` / `depth_stencil` target
  views (dimension-typed: 1d/2d/2d-array/3d/cube/cube-array); see [concepts/views.md](concepts/views.md).
  Still deferred:
  - **texel buffer views** — a format-decoded linear buffer (`Buffer<T>` / `samplerBuffer`);
  - the **render-pass / OMSetRenderTargets consumer** for the RTV/DSV views (the descriptors + heaps
    exist in the dx12 backend, but nothing binds them as output-merger targets yet);
  - the **binding path** (pipelines, descriptor groups/layouts, `command_list` binding) that consumes
    `raw_view`, plus **reflection-driven validation** of a view's `T`/access against the shader;
  - the **backend `raw_view` translation** (`switch` on `(access, shape)` → native descriptor) — no
    backend code exists for views yet;
  - the `raw_view` **name** is provisional (`raw_view` vs `raw_binding`).
- **Typed buffer wrapper:** `raw_buffer` is untyped — every view factory re-specifies the element type
  (`as_readwrite_buffer<T>()`, `as_vertex_buffer<T>()`, `as_index_buffer(format)`, …). Add a strongly-typed,
  templated `buffer<T>` (the analogue of `texture<Traits>` over `raw_texture`) that carries its element
  type / shape so the type is inferred once at creation and checked at compile time, rather than re-passed at
  each call site. It would wrap a `raw_buffer_handle` like `texture<Traits>` wraps `raw_texture_handle`.
- **Vertex attributes: go location-based, drop the HLSL semantic from the public API.** `vertex_attribute`
  currently identifies an input by an **HLSL `semantic` + `semantic_index` string** — the one identity that
  doesn't survive a change of shader language. Every other target matches vertex inputs by a **numeric
  location**: SPIR-V/Vulkan `layout(location=N)`, WGSL/WebGPU `@location(N)`, Metal `[[attribute(N)]]`;
  Vulkan's `VkVertexInputAttributeDescription` is literally `{location, binding, format, offset}` with no
  name. So the backend-neutral identity is a `u32 location`, and `{location, format, offset, slot}` is the
  union of the Vulkan / WebGPU / Metal models. Plan:
  - make `location` the attribute identity (replace `semantic` / `semantic_index` in `vertex_attribute`);
  - move the HLSL **semantic into `compiled_shader`'s reflected vertex-input signature** (already a deferred
    field there — the `// Deferred: … I/O signatures` note in `compiled_shader.hh`), as per-input
    `{location, semantic, semantic_index, format}`;
  - the **dx12 backend** then resolves `location → semantic` from that signature to fill
    `D3D12_INPUT_ELEMENT_DESC` (DX12 is the only backend that needs the string); SPIR-V / WGSL / Metal use
    `location` verbatim and ignore the semantic entirely;
  - optionally keep a semantic **hint** on the layout that is resolved to a location at pipeline-build time
    against the reflected VS input signature (ergonomic sugar for HLSL authors) — but the string is erased
    before it reaches any backend, so it never appears in the portable path.
  This is the direction; the current semantic-string form is an HLSL-only interim.
- **Blessed escape hatch:** add an sg API that returns raw underlying GPU handles without exposing
  the concrete backend types, so callers don't reach for `dynamic_cast` to a `sg::backend::*` type.
  See the [coding-guidelines](coding-guidelines.md) escape-hatch note.
- **SDK detection:** dx12 now links the Windows-SDK D3D12 libs (`d3d12 dxgi dxguid`) directly off
  the default lib path — good enough on the gated Windows path, but there's no explicit SDK
  presence/version check yet. vulkan gates on `find_package(Vulkan)` and links `Vulkan::Vulkan`; a
  version/feature floor beyond the 1.2 baseline is still worth adding.
- **Epoch system — deferred layers:** the epoch core (counter + direct-queue epoch/submission
  timelines, in-flight FIFO, advance/retire, throttle, deferred deletion + finalizers, command
  allocator/pool recycling) is in for dx12 and vulkan; see [concepts/epochs.md](concepts/epochs.md).
  Still deferred:
  - the **async copy queue** with pooled group fences and per-resource pending syncs (start:
    inline-only uploads on the direct queue);
  - **transient resources** — the linear bump allocator and the transient descriptor ring-buffers
    (start: persistent-only);
  - the **split GPU/CPU download watermarks** for readback (start: treat a download as done when the
    fence signals, with a synchronous CPU copy).
- **`cc::ringbuffer`:** the epoch in-flight set uses a `cc::vector` drained from the front, because
  `cc::ringbuffer` is currently an unimplemented stub. Switch to it once it lands.
- **Command-allocator pool as a standalone object:** dx12's `dx12_allocator_pool` is two vectors
  under one context-level `cc::mutex`. Promote it to an object that owns its synchronization and pools
  **per queue** (the epoch system grows multiple queues), instead of the ad-hoc mutex on the context.
- **Thread model nuance:** `sg::thread_model` is coarse (`single_threaded` / `multi_threaded`). Grow
  it as needed — e.g. whether concurrent command-list recording is allowed, or per-queue guarantees.
  See [concepts/threading.md](concepts/threading.md).
- **Tier 2 / legacy backends:** metal, webgpu, then opengl, webgl.
