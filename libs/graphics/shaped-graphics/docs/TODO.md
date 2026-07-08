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
  tracking + emission, since barrier models differ across backends. Still open: no **public texture op**
  (copy/upload/dispatch) drives the texture tracking yet; **texture views + shader-bound auto access**
  (`shader_layout_of` goes live then); **vulkan** barrier emission (lands with its compute/transfer
  milestone; it reuses the shared vocabulary + state machine); `declare_array_access` **full wiring** (API + validation are in, but applying it needs
  an array binding path + a binding-name→resource reflection map); migrating `access_flags` /
  `pipeline_stage_flags` to `cc::flags` when that lands; a per-draw/dispatch **escape hatch** that disables
  automatic transitions for callers that know their resources are already in the right layout; and folding
  the redundant `_open_command_lists` epoch-advance counter into the slot allocator's live count.
- **`cc::shared_ptr`:** the `*_handle` typedefs use `std::shared_ptr` as a placeholder. Surface a
  `cc::shared_ptr` in clean-core and switch handles to it (keeps sg off `std::`). See the
  [coding-guidelines](coding-guidelines.md) note.
- **`cc::flags`:** `buffer_usage` uses a hand-rolled `enum class` + bitwise operators; migrate to
  `cc::flags` once that clean-core type is implemented.
- **Views — deferred layers:** buffer views (`uniform`/`readonly`/`readwrite`, `byte` = raw) + the
  erased `raw_view` are in; see [concepts/views.md](concepts/views.md). Still deferred:
  - **texture + texel views** — their own view family, blocked on `sg::texture` + a pixel-`format`
    enum (dimension-typed: 1d/2d/2d-array/3d/cube/cube-array, + `render_target`/`depth_stencil`);
  - the **binding path** (pipelines, descriptor groups/layouts, `command_list` binding) that consumes
    `raw_view`, plus **reflection-driven validation** of a view's `T`/access against the shader;
  - the **backend `raw_view` translation** (`switch` on `(access, shape)` → native descriptor) — no
    backend code exists for views yet;
  - the `raw_view` **name** is provisional (`raw_view` vs `raw_binding`).
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
