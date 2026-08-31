# shaped-graphics TODO

Running list of known follow-ups — what is **open**.
What is already implemented is [structure.md](structure.md)'s tagged tree, and the design behind each area is its concept doc.

- **Transfer.** Still open:
  - **device→device texture copy** — `cmd.copy` does buffer regions only;
  - **fallback staging** when one list's inline transfers exceed the ring capacity.
    The ring blocks on in-flight epochs first, but with nothing in flight it asserts.
  - a **parallel host copy** for a large inline upload — take a `cc::pinned_data`, copy it on worker threads, and block at submit rather than inside `bytes_to_buffer`.
  - a **direction-specific async-ready layout**, which the transfer path cannot hold today.
    `async_ready_layout` returns `general` on both backends, so `sg::async_direction` is accepted and ignored — a caller's statement of intent rather than an answer.
    Vulkan could copy from `TRANSFER_SRC_OPTIMAL` and into `TRANSFER_DST_OPTIMAL` and keep whatever compression that buys, and dx12 could not: its copy queue needs COMMON either way.
    **What rules it out is submit-call order.**
    The fixup that settles a texture's layout runs at *enqueue*, on the calling thread, while a transfer already enqueued has not necessarily been submitted by its actor yet.
    An upload followed by a download of one texture therefore puts the download's fixup ahead of the upload's copy in call order.
    The validation layer tracks image layouts in `vkQueueSubmit` call order and models no semaphore, so it reads the upload's copy as naming a layout the image has left.
    The GPU ordering is correct throughout; only the layer disagrees, and a layer message fails a test.
    One layout for both directions removes the second fixup, and with it the interleave.
    The reproduction is [tests/transfer/stream-test.cc](../tests/transfer/stream-test.cc)'s `a texture sink receives whole tightly-packed rows`.
    It failed about one run in ten with direction-specific layouts, and passes 40/40 with one.
    **What would earn it back** is submitting the fixup from the transfer actor, immediately before the job it belongs to, so call order matches queue order.
    That needs a direct-queue submit from the actor thread and a job whose wait token is settled after the fact, which is why it is a follow-up rather than part of the change that found it.
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
- **An optional clear value on `texture_description`.**
  D3D12 takes a `D3D12_CLEAR_VALUE` at resource creation and uses it to pick a fast-clear path.
  On most hardware that means clear-colour compression metadata, valid only for the one value the resource was created with.
  Creating without it is legal and costs a slower clear, which is the `did not pass any clear value to resource creation` advisory both test listeners allowlist (`dx12_expected_messages.hh`).
  We pass `nullptr` everywhere today, and that is the right default rather than an oversight.
  sg lets any render pass clear to any colour (`rt.cleared(colour)`), so a stored value that disagrees with the actual clear is a *worse* outcome than none:
  D3D12 then raises `CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE` and takes the slow path anyway.
  So the shape is an **optional** field a caller opts into when the clear really is fixed.
  That is the common case for a depth target cleared to 1.0 every frame, and for a render target with a constant background.
  Vulkan has no creation-time equivalent, so it would be a hint one backend honours and the other ignores.
  Acceptable for a pure performance hint, and worth stating in [concepts/views.md](concepts/views.md) if it lands.
  Worth doing only with a measurement behind it: a fast clear is a bandwidth win on a full-screen target and noise on a small one.
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

- **A texture readback can come back all zeroes when two lists record concurrently on one context.**
  Seen only on vulkan, on Windows, while the tier-2 suite briefly shared one context.
  About one run in eleven, `vulkan-raster-test.cc`'s "a rendering scope clears, draws and stores" read its target back as 4096 zero bytes.
  Not another test's pixels and not a wrong colour — nothing from either render scope survived, and the clear alone would have left alpha at 255.
  No validation message accompanied it.
  Run alone the test passes 30/30, so it needs the concurrency.
  **The ring-ordering hypothesis is excluded.**
  `cmd.download`'s staging is reserved while a list records and its jobs are enqueued when a list submits.
  `vulkan_download_inline.hh` claims those orders are the same, and they are not once two lists are open.
  But each job carries its own `deferred_cpu_copy` closure over its own reservation, so drain order decides when a copy runs and never which window it reads.
  [tests/transfer/overlapping-readback-test.cc](../tests/transfer/overlapping-readback-test.cc) reserves in one order and submits in the other deliberately, and passes 20/20 on every backend.
  It stays as the gate for that.
  The tier-2 suite is back to a context per test, so nothing we ship reaches this today — which also means it cannot be reproduced on demand any more.

- **`prepare_for_async` — an async transfer of a texture cannot be interleaved with command lists that use it.**
  Both backends, for different reasons, and the fix is one design rather than two.

  **The defect.**
  An async transfer runs on a queue that cannot settle the resource's layout for itself.
  A D3D12 copy queue **cannot run layout barriers at all**, so a copy there requires the resource in `COMMON`; an inline list
  that left it in `COPY_DEST` makes the copy illegal, and the debug layer says so.
  Vulkan's transfer queue *can* barrier, and sg's borrow-and-restore makes the GPU ordering correct — but the validation layer
  tracks image layouts in `vkQueueSubmit` **call** order and does not model semaphores, so a transfer submit landing after a
  direct submit reads as a layout mismatch.
  Correct and unverifiable is still unshippable, since the test listener fails on any layer message.
  So today the supported pattern is a texture touched by *either* command lists *or* async transfers, never both around one
  transfer.

  **The agreed design — a prepare command, and a lock that makes it hold.**
  A resource is moved into its async-ready layout by the **direct** queue, at a point the caller picks, and then held there:
  - a command list records `prepare_for_async(resource, range)`, which declares an access like any other, so the transition
    rides the existing declare/flush path and needs no new barrier machinery;
  - the **backend** picks the target layout — `general` on dx12, which is what its copy queue requires, and `copy_src` /
    `copy_dst` on vulkan.
    The command says "prepare for an async transfer", never "transition to layout X";
  - once submitted, the range is **locked**: sg refuses the uses that would move the layout, so nothing can change it between
    that submit and the transfer's.
    A second command releases it.

  The lock is what does the work, and it is why an explicit prepare *without* it would not be enough: a prepare that only
  settles the layout at one instant leaves the same race, just moved.
  With the lock, the transfer needs no layout transition at all — `oldLayout == newLayout`, or no image barrier whatsoever —
  so there is no layout claim left to get wrong, and the layer's map agrees by construction.

  **What the lock must *not* forbid.**
  Writes and layout-changing uses, not all uses.
  A buffer has no layout, so a shader read on the direct queue concurrent with an async download is legal Vulkan today and
  needs nothing — the resources are already `VK_SHARING_MODE_CONCURRENT`, and the only precondition is no unsynchronized
  writer.
  Forbidding that would outlaw exactly the overlap the transfer queue exists to provide.
  A texture is different only because one image cannot be in two layouts at once: a prepared texture asserts on a concurrent
  shader read by default, and preparing to `general` instead keeps such reads legal at a compression cost.
  That is the same trade `combine_layouts` already makes when an SRV and a UAV collide on one op, warning included.

  **Other things it settles.**
  Buffers gain no correctness from it, but a mandatory direct-queue hop means async uploads and downloads can no longer chain
  transfer-to-transfer — which is what the cross-linked `upload_wait` / `download_wait` completion-group pairs exist to handle,
  and they can go.
  The state is per subresource, which is nearly free: the canonical state is already a covering partition over
  mip × array × aspect, so the lock is a flag beside `curr_layout` / `prev_layout`.
  Set and cleared at **submit**, under the texture's mutex in submission order, exactly as the canonical promote already is —
  so it composes with concurrent recording for the same reason that does.

  **The gate.**
  Three tier-1 tests, written and then held back because neither backend passes them:
  - `sg - async upload composes after a list that wrote the texture` — an inline list writes the texture and submits, then an
    async upload of a different pattern must compose *after* it (the reverse edge);
  - `sg - a list reading a texture waits on an in-flight async upload` — a seeded texture, an async upload, then an inline
    readback recorded straight afterwards must see the streamed bytes (the forward edge);
  - `sg - an async texture transfer hands the texture back in the layout it found` — an inline write, a `ctx.download`, then a
    second inline write and readback.
    This one **passes on vulkan today** and is kept as
    [backends/vulkan/tests/vulkan-texture-transfer-test.cc](../backends/vulkan/tests/vulkan-texture-transfer-test.cc);
    it moves up to tier 1 once dx12 can hold it too.

  The ergonomic cost is real and worth taking: a one-shot `ctx.download.bytes_from_texture` needs no command list today and
  would need a prepare recorded on one.
  An unprepared resource should be **refused** rather than silently fixed by a just-in-time transition, since a silent fix
  reintroduces the race the lock exists to remove.

- **Tier 2 / legacy backends:** metal, webgpu, then opengl, webgl.
