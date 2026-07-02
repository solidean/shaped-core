# shaped-graphics TODO

Running list of known follow-ups. Bigger design intent lives in
[structure.md](structure.md).

- **First milestone:** command-list buffer upload / download / copy (sg core API + real dx12 and
  vulkan implementations). Everything in the core is currently a `CC_UNREACHABLE` stub.
- **`cc::shared_ptr`:** the `*_handle` typedefs use `std::shared_ptr` as a placeholder. Surface a
  `cc::shared_ptr` in clean-core and switch handles to it (keeps sg off `std::`). See the
  [coding-guidelines](coding-guidelines.md) note.
- **`cc::flags`:** `buffer_usage` uses a hand-rolled `enum class` + bitwise operators; migrate to
  `cc::flags` once that clean-core type is implemented.
- **Blessed escape hatch:** add an sg API that returns raw underlying GPU handles without exposing
  the concrete backend types, so callers don't reach for `dynamic_cast` to a `sg::backend::*` type.
  See the [coding-guidelines](coding-guidelines.md) escape-hatch note.
- **SDK detection:** dx12 now links the Windows-SDK D3D12 libs (`d3d12 dxgi dxguid`) directly off
  the default lib path — good enough on the gated Windows path, but there's no explicit SDK
  presence/version check yet. vulkan is still a stub; add `find_package(Vulkan)` + availability
  gating when it gets a real implementation.
- **Epoch system — deferred layers:** the epoch core (counter + direct-queue epoch/submission
  fences, in-flight FIFO, advance/retire, throttle, deferred deletion + finalizers, command-allocator
  recycling) is in for dx12; see [concepts/epochs.md](concepts/epochs.md). Still deferred:
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
