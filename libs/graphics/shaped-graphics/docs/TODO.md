# shaped-graphics TODO

Running list of known follow-ups — what is **open**.
What is already implemented is [structure.md](structure.md)'s tagged tree, and the design behind each area is its concept doc.

- **Transfer.** Still open:
  - **device→device texture copy** — `cmd.copy` does buffer regions only;
  - **fallback staging** when one list's inline transfers exceed the ring capacity.
    The ring blocks on in-flight epochs first, but with nothing in flight it asserts.
  - a **parallel host copy** for a large inline upload — take a `cc::pinned_data`, copy it on worker threads, and block at submit rather than inside `bytes_to_buffer`.
- **Barriers + access tracking.** See [concepts/barriers.md](concepts/barriers.md). Still open:
  - **array bindings in raster draws** — compute/RT dispatches resolve `declare_array_*_access` against the bound groups, but the raster scope has no declare pair and asserts on a bound array binding;
  - a per-draw/dispatch **escape hatch** disabling automatic transitions where the caller knows its resources are already in the right layout;
  - folding the redundant `_open_command_lists` epoch-advance counter into the slot allocator's live count.
- **Raster pipeline + draws.** See [concepts/raster-pipeline.md](concepts/raster-pipeline.md). Still open:
  - **indirect draws** — `draw_indirect` and count buffers;
  - **dynamic primitive topology** and **dynamic depth bias**, both baked into the PSO for now;
  - **mesh / task** stages;
  - a **backend-neutral numeric `location`** on `sg::vertex_attribute`, replacing the HLSL `semantic` string.
    The vulkan backend currently numbers a SPIR-V location by an attribute's index in `vertex_input_layout::attributes`.
    That makes the shader's `[[vk::location(N)]]` annotations part of the contract — see `vulkan_raster_pipeline.cc`.
- **Acceleration structures.** See [concepts/acceleration-structures.md](concepts/acceleration-structures.md).
  The abstract types already carry the stats a refit needs — build and update scratch sizes, flags, the storage handle.
  Still open:
  - the **transient (single-epoch) AS variant** for per-frame rebuilds — a property of the build call's result, not a new scope;
  - **refit / update** — reuses the topology, and needs `allow_update` at build plus `PERFORM_UPDATE` and the source AS at update time;
  - **compaction** — BLAS `allow_compaction`, query the compacted size, copy into a smaller buffer;
  - **compaction** on both backends, which is the one build-time flag neither implements.
- **Raytracing pipeline.** The dx12 trace path is in — see [concepts/raytracing-pipeline.md](concepts/raytracing-pipeline.md).
  Still open: **local root signatures** and a **state-object cached blob**.
  Plus a **dedicated shader-table buffer**: `raytracing_shader_table` exists, but its records sit in a plain shader-readable buffer as a stand-in.
  [types.hh](../src/shaped-graphics/types.hh) rules an SBT out of `buffer_usage` deliberately, so the storage needs a type of its own.
- **`cc::shared_ptr`:** the `*_handle` typedefs still use `std::shared_ptr`.
  [`cc::shared_ptr`](../../../base/clean-core/src/clean-core/memory/shared_ptr.hh) exists — 8 B, intrusive, Traits-keyed.
  But its Traits protocol is provisional, shaped by `cc::async`'s needs and expected to be simplified.
  So this is gated on that API settling rather than ready to pick up: see [systems/shared-ptr](../../../base/clean-core/docs/systems/shared-ptr.md).
  It will not be a drop-in even then.
  sg's resources are polymorphic, so `default_shared_traits`' `sizeof(T)`-derived control offset cannot find the counts through a base-typed handle — the same blocker slib hits.
  They also derive from `std::enable_shared_from_this`, with 30+ `shared_from_this()` call sites and no `cc::shared_ptr` equivalent.
  See the [coding-guidelines](coding-guidelines.md) note.
- **`cc::atomic`:** sg still names `std::atomic` / `std::memory_order` directly.
  About 110 occurrences, across the dx12 and vulkan backends, `raw_buffer`, `raw_texture`, `bytes_future` and `acceleration_structure`.
  clean-core has migrated to [`cc::atomic`](../../../base/clean-core/src/clean-core/thread/atomic.hh), and `<atomic>` is no longer blessed to call into directly.
  See [blessed-stdlib-headers.md](../../../base/clean-core/docs/blessed-stdlib-headers.md).
  The migration is mechanical, since with threads `cc::atomic` **is** `std::atomic`.
  It becomes load-bearing when WebGPU-on-wasm lands: that build has no threads, and every one of those atomics would keep its interlock for a concurrency that cannot happen.
- **Views.** See [concepts/views.md](concepts/views.md). Still deferred:
  - **texel buffer views** — a format-decoded linear buffer (`Buffer<T>` / `samplerBuffer`);
  - **reflection-driven validation** of a view's `T` and access class against the shader;
  - the `raw_view` **name** is provisional (`raw_view` vs `raw_binding`).
- **Vertex attributes: go location-based, drop the HLSL semantic from the public API.**
  `vertex_attribute` identifies an input by an **HLSL `semantic` + `semantic_index` string** — the one identity that does not survive a change of shader language.
  Every other target matches vertex inputs by a **numeric location**: SPIR-V/Vulkan `layout(location=N)`, WGSL/WebGPU `@location(N)`, Metal `[[attribute(N)]]`.
  Vulkan's `VkVertexInputAttributeDescription` is literally `{location, binding, format, offset}`, with no name.
  So the backend-neutral identity is a `u32 location`, and `{location, format, offset, slot}` is the union of the Vulkan / WebGPU / Metal models.
  Plan:
  - make `location` the attribute identity, replacing `semantic` / `semantic_index` in `vertex_attribute`;
  - move the HLSL **semantic into `compiled_shader`'s reflected vertex-input signature** as per-input `{location, semantic, semantic_index, format}` — already a deferred field there;
  - the **dx12 backend** then resolves `location → semantic` from that signature to fill `D3D12_INPUT_ELEMENT_DESC`, since DX12 is the only backend that needs the string.
    SPIR-V / WGSL / Metal use `location` verbatim and ignore the semantic entirely;
  - optionally keep a semantic **hint** on the layout, resolved to a location at pipeline-build time against the reflected VS input signature — ergonomic sugar for HLSL authors.
    The string is erased before it reaches any backend, so it never appears in the portable path.
- **Blessed escape hatch:** an sg API returning the raw underlying GPU handles without exposing the concrete backend types, so a caller never reaches for `dynamic_cast` to an `sg::backend::*` type.
  See the [coding-guidelines](coding-guidelines.md) escape-hatch note.
- **SDK detection:** dx12 links the Windows-SDK D3D12 libs (`d3d12 dxgi dxguid`) straight off the default lib path, with no explicit SDK presence or version check.
  vulkan gates on `find_package(Vulkan)` and links `Vulkan::Vulkan`; its device floor is 1.3 plus descriptor_buffer and robustness2, refused by name at creation.
- **Epoch system.** See [concepts/epochs.md](concepts/epochs.md). Still deferred:
  - a **texture-capable transient heap** — `ctx.transient`'s bump allocator is buffers-only, so a transient texture falls back to a dedicated allocation the backend auto-expires at the next epoch.
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
- **Swapchain / presentation.** See [concepts/presentation.md](concepts/presentation.md).
  Both backends are real, windowed and headless.
  Still open:
  - a **cocoa arm on `sg::window_platform`**, for the metal backend that would consume it — see shaped-rendering's [TODO](../../shaped-rendering/docs/TODO.md);
  - **deeper HDR** — metadata and tone-mapping beyond the colorspace set.
    Including whether the request was *granted*: `enable_hdr` is best-effort on both backends and `is_hdr_enabled()`
    reports what was asked for, so nothing tells a caller which colorspace it actually got;
  - **exclusive fullscreen** and **multi-window**;
  - letting a windowed renderer thread the swapchain's back-buffer count into `advance_epoch`;
- **A shared async pool can outlive the device a node's value belongs to.**
  Seen once, under a full `check` (five presets building and testing at once): `vkDestroyDevice` reported two
  `VkPipeline`s and their `VkPipelineCache`s leaked, from a tier-2 test that had already dropped every handle to them.
  The pipeline cache releases its providers at shutdown, so the only remaining owner is the `cc::async` node the build
  ran on — and the pool is process-wide while a device is per test.
  Not reproduced in isolation: 40 repeats of that test, three full-suite runs and a second `check` are all green, so it
  needs the contention.
  The single-threaded pool had the same shape and was fixed by dropping finished nodes in `participate_until_ready`;
  whether the threaded one retains a finished node anywhere is the thing to establish.

- **Tier 2 / legacy backends:** metal, webgpu, then opengl, webgl.
