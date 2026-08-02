# shaped-graphics TODO

Running list of known follow-ups. Bigger design intent lives in
[structure.md](structure.md).

- **Transfer — remaining:** host↔device transfer is in for **buffers and textures**, both inline
  (`cmd.upload` / `cmd.download`, over dx12's UPLOAD / READBACK ring buffers) and async
  (`ctx.upload` / `ctx.download`, over dedicated copy queues with cross-queue fence waits), with
  `bytes_future` as the shared result.
  Still open: the **vulkan** implementation (currently a
  `CC_UNREACHABLE` stub); **device→device texture copy** (`cmd.copy` only does buffer regions today);
  and **fallback staging** when a single list's inline transfers exceed the ring capacity — the ring
  now blocks on in-flight epochs first, but with nothing in flight it still asserts.
- **Barriers + access tracking — remaining:** the access-tracking system is in for **buffers** (inferred
  access, the three-timeline `resource_access_state`, the command-list slot model with revert/promote, and
  dx12 enhanced-barrier emission — see [concepts/barriers.md](concepts/barriers.md)). **Textures** are
  tracked too: each `dx12_texture` owns a per-command-list covering partition and emits subresource-range
  `D3D12_TEXTURE_BARRIER` layout transitions (with entry-layout revert on a non-final submit). Real public
  ops drive it — texture upload/download, a bound texture in a compute dispatch (SRV/UAV views transition
  via `shader_layout_of`), and the rendering scope's color / depth-stencil targets.
  Still open: **vulkan**
  barrier emission (lands with its compute/transfer milestone; it reuses the shared vocabulary + state
  machine); `declare_array_access` **full wiring** (API + validation are in, but applying it needs an array
  binding path + a binding-name→resource reflection map); migrating `access_flags` /
  `pipeline_stage_flags` to `cc::flags` when that lands; a per-draw/dispatch **escape hatch** that disables
  automatic transitions for callers that know their resources are already in the right layout; and folding
  the redundant `_open_command_lists` epoch-advance counter into the slot allocator's live count.
- **Raster pipeline + draws — deferred layers:** the graphics path is in (`sg::raster_pipeline` +
  `raster_pipeline_description` with its fixed-function state vocabulary, geometry + tessellation stages,
  `ctx.uncached.create_raster_pipeline`, draw recording on `cmd.raster` / `cmd.raster.manual`, and the
  rendering scope that binds color / depth-stencil targets; dx12 real on WARP, vulkan stubbed). See
  [concepts/raster-pipeline.md](concepts/raster-pipeline.md). Still open: **PSO caching**
  (`ctx.cached.acquire_raster_pipeline` + `pipeline_cache` description hashing + `async_raster_pipeline` —
  the compute/RT parity piece); **indirect draws** (`draw_indirect` / count buffers); **dynamic primitive
  topology** and **dynamic depth bias** (both baked into the PSO for now); **mesh / task** stages; and the
  **vulkan** implementation (`VkPipeline` + dynamic-rendering formats + the `vkCmdDraw*` seams — currently
  `CC_UNREACHABLE`).
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
- **`cc::shared_ptr`:** the `*_handle` typedefs use `std::shared_ptr` as a placeholder.
  [`cc::shared_ptr`](../../../base/clean-core/src/clean-core/memory/shared_ptr.hh) has landed (8 B, intrusive,
  Traits-keyed), so what remains is switching the handles to it (keeps sg off `std::`). Needs a Traits for the
  sg shapes — `cc::default_shared_traits` gives a trailing control block with no source change. See the
  [coding-guidelines](coding-guidelines.md) note.
- **`cc::atomic`:** sg still names `std::atomic` / `std::memory_order` directly (~110 occurrences across the
  dx12 + vulkan backends, `raw_buffer`, `raw_texture`, `bytes_future`, `acceleration_structure`). clean-core
  has migrated to [`cc::atomic`](../../../base/clean-core/src/clean-core/thread/atomic.hh), and `<atomic>` is
  no longer blessed to call into directly — see
  [blessed-stdlib-headers.md](../../../base/clean-core/docs/blessed-stdlib-headers.md). The migration is
  mechanical (with threads `cc::atomic` **is** `std::atomic`), and no correctness gap exists today because
  dx12/vulkan are desktop-only. It becomes real when WebGPU-on-wasm lands: that build has no threads, and
  every one of those atomics would keep its interlock for a concurrency that cannot happen.
- **`cc::flags`:** `buffer_usage` and `texture_usage` use a hand-rolled `enum class` + bitwise operators;
  migrate to `cc::flags` once that clean-core type is implemented (it is still a stub).
- **Views — deferred layers:** buffer views (`uniform`/`readonly`/`readwrite`, `byte` = raw) + the
  erased `raw_view` are in, as are texture SRV/UAV views and `render_target` / `depth_stencil` target
  views (dimension-typed: 1d/2d/2d-array/3d/cube/cube-array), which the rendering scope binds as
  output-merger targets; see [concepts/views.md](concepts/views.md). Still deferred:
  - **texel buffer views** — a format-decoded linear buffer (`Buffer<T>` / `samplerBuffer`);
  - **reflection-driven validation** of a view's `T`/access against the shader;
  - the `raw_view` **name** is provisional (`raw_view` vs `raw_binding`).
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
  allocator/pool recycling) is in for dx12 and vulkan, as are dx12's dedicated **async copy queues**
  behind `ctx.upload` / `ctx.download`; see [concepts/epochs.md](concepts/epochs.md).
  Still deferred:
  - the **vulkan** async copy queue, which has neither the queue nor the per-resource pending syncs;
  - **transient textures** — `ctx.transient`'s bump allocator and the transient descriptor ring are in
    for dx12, but the heap is buffers-only, so a transient texture still asserts.
- **`cc::ringbuffer`:** the epoch in-flight set uses a `cc::vector` drained from the front, because
  `cc::ringbuffer` is currently an unimplemented stub. Switch to it once it lands.
- **Render routines want a shared/exclusive lock, not a mutex.**
  The model to reach is: a routine's init phases exclude every `execute`, while `execute` calls that only *read* run in parallel with each other.
  A read-only routine like `sr::blit_routine` — one that can be acquired without exclusivity — has no reason to serialize against another thread's `execute`.
  Both halves are approximated today, because clean-core has no shared/exclusive mutex:
  `acquire()` takes **no** lock where it wants a shared one (so a reload's `init_declare` can run while it reads), and `acquire_exclusive()` serializes `execute` calls that would be free to overlap.
  The clean-core extension it needs is a `cc::shared_mutex<T>` next to `cc::mutex<T>` — `lock_shared(f)` / `lock_shared_scoped()` alongside `lock(f)` / `lock_scoped()`.
  Then `acquire()` holds a shared guard for the caller's read, `acquire_exclusive()` keeps the exclusive one, and the init phases run under the exclusive side of the same lock.
  See [render_routine.hh](../src/shaped-graphics/routine/render_routine.hh) and [render-routines.md](render-routines.md#threading).
- **Thread model nuance:** `sg::thread_model` is coarse (`single_threaded` / `multi_threaded`). Grow
  it as needed — e.g. whether concurrent command-list recording is allowed, or per-queue guarantees.
  See [concepts/threading.md](concepts/threading.md).
- **Swapchain / presentation — deferred layers:** the windowed-presentation path is in
  (`ctx.create_swapchain` -> `sg::swapchain` with `acquire_backbuffer` / `present` / `get_size` +
  description getters; auto-resize on acquire; `present_mode` vsync/immediate; an `enable_hdr` flag). dx12
  is real (an `IDXGISwapChain3` flip-discard chain; back buffers wrapped as `dx12_texture` so they ride the
  normal render-pass + barrier path; a dedicated present fence gates buffer reuse; `present()` transitions
  the buffer to `texture_layout::present` via `dx12_command_list::transition_texture_to`). The native
  window handle is an opaque `void*` (HWND on Windows) in the agnostic description. Still open: the
  **vulkan** implementation (`VkSurfaceKHR` + `VkSwapchainKHR` + acquire/present semaphores — currently a
  not-implemented `cc::error` stub); a **proper `native_window_handle` type** to replace the opaque `void*`
  in `swapchain_description` (a small platform-tagged struct — HWND / xcb+window / wl_surface / NSWindow —
  once a second windowing backend actually needs one, so it isn't speculative); **deeper HDR** (metadata /
  tone-mapping beyond the colorspace set); **exclusive fullscreen** and **multi-window**; letting a windowed
  renderer thread the swapchain's back-buffer count into `advance_epoch`; and a headless/offscreen present
  target so the present path can be pixel-verified on CI (today the WARP swapchain test needs a real hidden
  window and SKIPs without one).
- **Tier 2 / legacy backends:** metal, webgpu, then opengl, webgl.
