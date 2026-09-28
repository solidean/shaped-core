# shaped-graphics TODO

Running list of known follow-ups — what is **open**.
What is already implemented is [structure.md](structure.md)'s tagged tree, and the design behind each area is its concept doc.

- **A declared native scope, so foreign code can record onto an sg command list.**
  Vendor SDKs (DLSS Ray Reconstruction, FSR Ray Regeneration) take the native device, list and resources, and assume the resources are already in the state they need.
  Nothing public reaches a native handle today, and reaching into a backend's privates would bypass the barrier tracker silently.
  The shape: `sg::dx12::native_scope::open(cmd, accesses)` names every resource the foreign code touches and how, in the neutral `access_flags` vocabulary, and emits their barriers.
  It then hands out the native list, device and resources, asserting on one that was not declared.
  Closing it records the declared states as current and invalidates the list's cached pipeline, heap and root bindings.
  It is a second member of "access is inferred, never declared (with one exception)", for the same reason the bindless declaration is the first.
  dx12 first, vulkan when a member needs it; sr's [denoising.md](../../shaped-rendering/docs/denoising.md) is the consumer.
  Pin it with a test that clears through the scope and checks sg's next inferred barrier.
- **Exportable memory and shared fences.**
  OIDN's GPU devices run on their own API (CUDA, HIP, SYCL, Metal) and share memory with ours through an OS handle.
  That wants an "exportable" usage on buffer and texture creation, a way to read the handle, and a fence shared both ways.
  Not needed for OIDN on the CPU, which goes through the existing download and upload; built with the OIDN member.
- **A pipeline-level static sampler (`bound_sampler`) is bound by dx12 and webgpu only.**
  vulkan created the `VkSampler`s and bound them to no set, and metal read `static_samplers` not at all, so a shader sampling through one read nothing.
  Both now refuse a pipeline layout that carries one, rather than building a pipeline that samples garbage.
  Closing it is a reserved descriptor set of immutable samplers on vulkan, at `sg::reserved_binding_group` as webgpu has it, and the same argument buffer slot on metal.
  SGL's file-scope `sampler name:` waits on this, and a group's name-matched static sampler is what works everywhere meanwhile.

- **The metal backend serializes no pipeline blob.**
  `compute_pipeline::cached_pipeline_data()` returns empty there and `used_cached_pipeline()` is always false, so a
  caller persisting a blob across runs gets nothing to persist and every build is a cold one.
  It is not a missing call: sg's surface is **one blob per pipeline**, and Metal 4's `MTL4Archive` is **one store per
  compiler** that accumulates every pipeline built through it.
  dx12 hands back a `ID3D12PipelineState` blob and vulkan gives each pipeline a `VkPipelineCache` of its own; neither
  shape exists here.
  The options are an archive per context serialized as a whole (which sg's per-pipeline key cannot address), an
  archive per pipeline (one compiler each, which is heavy), or a surface change so a backend may own the store.
  Pinned as deliberate by `sg metal - a compute pipeline builds from a metal library`, so closing it is a failing test
  rather than something nobody notices.

- **`context::_device_lost` is a plain bool, and metal writes it from a driver thread.**
  Every other piece of sticky context state is guarded or atomic; this one is a bare `bool` set by `mark_device_lost`
  and read by `is_device_lost` on any thread.
  It was sound while no backend wrote it from a thread sg does not own.
  Metal is the first that does: its only error channel is the `MTL4CommitFeedback` handler, which runs on a dispatch
  queue Apple owns, and that handler calls `report_feedback_error` and so `mark_device_lost`.
  dx12 polls `GetDeviceRemovedReason` from the calling thread and vulkan's debug callback normally arrives on it too,
  so neither backend exposed this.
  The race is benign in practice — a sticky flag written once with one value — and it is still a data race a
  sanitizer is entitled to report, on a field every backend reads per frame.
  The fix is `cc::atomic<bool>` plus the release/acquire pair, which costs nothing on the read path; what makes it a
  question rather than a patch is that `SC_THREADS=OFF` turns `cc::atomic` back into a plain value, and a driver
  thread does not go away with that flag — so it may want the same treatment metal's own callback state got.

- **Transfer.** Still open:
  - **device→device texture copy** — `cmd.copy` does buffer regions only;
  - **fallback staging** when one list's inline transfers exceed the ring capacity.
    On dx12 and vulkan the ring blocks on in-flight epochs first, but with nothing in flight it asserts; webgpu stages the overflow in a one-off buffer and warns.
  - a **parallel host copy** for a large inline upload — take a `cc::pinned_data`, copy it on worker threads, and block at submit rather than inside `bytes_to_buffer`.
  - **an async transfer does not order against an in-flight *stream* of the same resource.**
    Command lists do: their access tracking reads the stream stamps alongside the async ones, waits, and warns once per stream.
    The async tier does not — a download job reads `_pending_async_upload_value` and no stream value, and the upload side mirrors that.
    So `ctx.stream.bytes_to_buffer` followed by `ctx.download.bytes_from_buffer` on one resource is unordered, and the readback can beat the stream.
    Found while writing [tests/transfer/stream-test.cc](../tests/transfer/stream-test.cc)'s stream-wait test, whose first draft used the async tier as the consumer and read zeroes on dx12.
    The fix mirrors what the command lists already do, in dx12's async enqueue paths.
    Metal already does it: its streaming timeline is per resource, so an async transfer waits on the same value a
    command list would — which is also what makes `promote_to_async` a pure statement of intent there.
    The reverse edge there — a *stream* ordering behind an in-flight async transfer — was missing until the tier-1
    sweep read zeroes out of a texture an async upload was still filling, and `order_stream_copy` now waits on the
    transfer timeline as well.
  - **dx12 reads a texture's pending transfers when a list records, not when it submits.**
    `dx12_command_list::track_texture_access` folds the upload and download waits in at the op, and picks `general`
    only if a transfer is pending then.
    A list recorded before an async or streaming upload is enqueued and submitted after it therefore does not wait for
    the copy, and transitions the texture out of the layout the copy queue is still using — the debug layer reports
    it, and the list reads the texture before the upload lands.
    That is the "a check taken during recording" defect [barriers](concepts/barriers.md#what-orders-what-the-calls-on-the-context)
    names; vulkan gathers the waits at submit and does not have it, and neither backend yet revisits the layout there.
    `sg stream - a list recorded before a fresh texture's upload and submitted after it` pins the vulkan side and skips
    dx12 until this is fixed.
  - **a pure layout transition is modelled as touching nothing**, so nothing orders against it.
    `cmd.ensure_layout` — and the async fixup, which is one — declares no stage and no access, since it asks for a layout and nothing else.
    The barrier that produces therefore has an empty scope on both sides, and two things follow from that.
    Its destination scope orders nothing after it within its own submit.
    And the state it commits records no write, so the *next* list's entry barrier is computed against a timeline that has forgotten the transition happened.
    Synchronization validation reports the second one as a `READ_AFTER_WRITE` against "a prior layout transition".
    **The missing gate case is the forward edge read through an inline readback**: an async upload, then a list recorded straight afterwards, on a texture the fixup transitioned.
    Its bytes are right on both backends — the semaphores order it correctly — and only the layer disagrees.
    [tests/transfer/texture-async-interleave-test.cc](../tests/transfer/texture-async-interleave-test.cc) carries the rest of the gate and would carry this one too.
    **The fix is to model a pure transition as a write at full scope**, which a transition physically is.
    Neither `pipeline_stage_flags` nor `access_flags` spells "all", and naming every stage is not the answer either.
    The vulkan initial-transition prepend deliberately avoids that, since a ray-tracing stage is invalid on a device without the extension.
    So it wants a marker on `access_barrier` and on the in-flight state — "full scope, widen at emission".
    Each backend already knows how to spell that: `ALL_COMMANDS` plus `MEMORY_READ | MEMORY_WRITE` on vulkan.
    **The vulkan present path pays for this today, and the workaround is a wait mask.**
    A presenting submit waits the acquire semaphore at `ALL_COMMANDS` rather than at `COLOR_ATTACHMENT_OUTPUT`, the stage that actually writes the back buffer.
    A wait dst stage orders that stage and later ones, and the back buffer's entry transition runs ahead of every stage in the prepended buffer.
    So the narrower mask left the transition unordered against the acquire — a `WRITE_AFTER_READ` against `vkAcquireNextImageKHR`.
    Once a transition carries a full scope of its own, the mask can go back to naming the stage the work is in.
  - a **direction-specific async-ready layout** — postponed for simplicity, not blocked.
    `async_ready_layout` returns `general` on both backends, so `sg::async_direction` is accepted and ignored — a caller's statement of intent rather than an answer.
    Vulkan could copy from `TRANSFER_SRC_OPTIMAL` and into `TRANSFER_DST_OPTIMAL` and keep whatever compression that buys, and dx12 could not: its copy queue needs COMMON either way.
    **One layout everywhere is what needs no implementation.**
    It is slower in general and it is the reason the direction stays in the API: the shape extends easily and would be hard to add back.
    **What the naive attempt runs into is submit-call order.**
    The fixup that settles a texture's layout runs at *enqueue*, on the calling thread, while a transfer already enqueued has not necessarily been submitted by its actor yet.
    An upload followed by a download of one texture therefore puts the download's fixup ahead of the upload's copy in call order.
    The validation layer tracks image layouts in `vkQueueSubmit` call order and models no semaphore, so it reads the upload's copy as naming a layout the image has left.
    The GPU ordering is correct throughout; only the layer disagrees, and a layer message fails a test.
    One layout for both directions removes the second fixup, and with it the interleave.
    The reproduction is [tests/transfer/stream-test.cc](../tests/transfer/stream-test.cc)'s `a texture sink receives whole tightly-packed rows`.
    It failed about one run in ten with direction-specific layouts, and passes 40/40 with one.
    **What earns it back is ordering the fixups rather than avoiding them.**
    Submission and the async / stream entry points are all serialized against each other already.
    An upload and a download of one resource cannot overlap either, since each waits on the other.
    So the transitions a transfer needs can be inserted on the direct queue in that same order, which makes call order match queue order and the interleave impossible.
    That is a real piece of work: a direct-queue submit placed against each job rather than at enqueue.
    So it is a quality-of-implementation follow-up rather than part of the change that found it.
  - **a list recorded before a transfer and submitted after it strands that transfer's layout on dx12.**
    Reproduced, on WARP and on hardware:

    ```
    auto cmd = ctx->create_command_list();
    cmd->ensure_layout(tex, sg::texture_layout::shader_readonly);  // entry requirement recorded
    ctx->upload.bytes_to_texture(tex, pinned);                     // fixup settles COMMON, job enqueued
    ctx->submit_command_list(cc::move(cmd));                       // entry barrier moves it to SHADER_RESOURCE
    ```

    The copy queue then reports `Barrier layout(D3D12_BARRIER_LAYOUT_SHADER_RESOURCE) ... must be in expected layout (D3D12_BARRIER_LAYOUT_COMMON)`.
    A D3D12 copy queue cannot run a layout barrier at all, so the copy needs COMMON and the list took it away.
    Vulkan survives the same sequence — the semaphore orders it and the layer accepts it — so this is dx12-only today.

    **`has_pending_transfer` is what should have caught it, and cannot.**
    Both backends' `track_texture_access` force a texture to `general` while any transfer stamp is unreached.
    But it is consulted while *recording*, and no transfer was pending then — the enqueue comes afterwards.
    That is not a timing accident.
    sg's happens-before model is the order of calls on the *context*, and recording is none of those events, so a value read there answers a question the model does not pose.
    [concepts/barriers.md](concepts/barriers.md#what-orders-what-the-calls-on-the-context) is the contract.

    **The fix is the one the direction-specific entry above already names.**
    Submit the fixup from the transfer actor, immediately before the job it belongs to, so a list submitting in between cannot get underneath it.
    Clamping the entry layout at finalize instead does not work — a vulkan copy command names its layout literally, captured at record, so the body and the entry barrier would disagree.
    Until then the guard is worth neither trusting nor deleting: rewrite it in terms of context-call order, or remove it with the fix.
  - **an async download could cancel as soon as nobody can observe it.**
    A readback's job owns its source, so dropping every handle to the resource never cancels a download the caller still holds a future for.
    That is settled semantics, and [tests/transfer/download-async-test.cc](../tests/transfer/download-async-test.cc) pins it.
    Dropping the *future* does cancel, and today that is noticed when the actor next picks the job up.
    Finer would be to notice it per window and stop mid-copy, releasing the source with it.
    Pure quality of implementation: the bytes are unobservable either way, and what it buys is releasing a large source sooner.
- **Barriers + access tracking.** See [concepts/barriers.md](concepts/barriers.md). Still open:
  - **array bindings in raster draws** — the one gap here that a real renderer will hit, so it is spelled out rather than listed.
    `declare_array_buffer_access` / `declare_array_texture_access` live on the compute scope and the raytracing scope alone.
    `command_list_raster_scope` has neither, and there is no `raster_declare_array_*` virtual for one to dispatch to.
    A dispatch therefore resolves its declares against the bound groups, and a draw cannot.
    dx12, vulkan and metal each assert `"array bindings are not supported in raster draws yet"` on a bound array binding.
    [concepts/bindings.md](concepts/bindings.md#array-bindings) states that refusal as the contract.
    webgpu has no binding arrays at all, so there is nothing there to refuse.
    **What it costs is any bindless material table on a draw.**
    sv's tables work today only because it path-traces, declaring them through `cmd.raytracing` in `gpu_resource_manager`; the moment a raster path wants one it stops at this assert.
    Closing it is the declare pair on the raster scope, a `raster_declare_array_*` virtual, and the resolution in three backends — the compute path's shape, at the vertex and fragment stages.
    webgpu would have to gain binding arrays first.
    Nothing subtle blocks it; it has simply never been the blocking thing.
  - a per-draw/dispatch **escape hatch** disabling automatic transitions where the caller knows its resources are already in the right layout;
  - folding the redundant `_open_command_lists` epoch-advance counter into the slot allocator's live count.
- **Raster pipeline + draws.** See [concepts/raster-pipeline.md](concepts/raster-pipeline.md). Still open:
  - **indirect draws** — `draw_indirect` and count buffers;
  - **dynamic primitive topology** and **dynamic depth bias**, both baked into the PSO for now;
  - **mesh / task** stages;
  - a **backend-neutral numeric `location`** on `sg::vertex_attribute`, replacing the HLSL `semantic` string.
    The vulkan backend currently numbers a SPIR-V location by an attribute's index in `vertex_input_layout::attributes`.
    That makes the shader's `[[vk::location(N)]]` annotations part of the contract — see `vulkan_raster_pipeline.cc`.
  - **a metal shader package, so `rotating-cube` can grow a metal arm and `metal-cube` can retire.**
    `SC_EXAMPLE_BACKEND` now takes `metal`, and `examples/graphics/metal-cube` is a runnable windowed cube on it.
    It is a sibling of `rotating-cube` rather than a case of it, and that split is the open part.
    The cause is slib: `sc_add_shader_package` speaks `hlsl` and `wgsl`, and nothing in it speaks metal.
    HLSL is no way out either, since DXC publishes no macOS build, so there is no compiler on the host to turn rotating-cube's own source into something a metal context accepts.
    So `metal-cube` embeds a metallib compiled ahead of time by `xcrun metal`, the way the tier-2 fixtures do.
    It hand-writes the vertex layout and the constants block that a package would have generated.
    Closing it means a `metal` language for `sc_add_shader_package` that builds a `.metallib` and embeds it, plus the slib compiler seam that hands the blob back at `acquire`.
    `rotating-cube` then lists `metal` in its `SUPPORTS`, and the sibling example goes away along with its copy of the geometry and camera maths.
- **Acceleration structures.** See [concepts/acceleration-structures.md](concepts/acceleration-structures.md).
  The abstract types already carry the stats a refit needs — build and update scratch sizes, and the flags.
  Still open:
  - the **transient (single-epoch) AS variant** for per-frame rebuilds — a property of the build call's result, not a new scope;
  - **refit / update** — reuses the topology, and needs `allow_update` at build plus `PERFORM_UPDATE` and the source AS at update time;
  - **compaction** — BLAS `allow_compaction`, query the compacted size, copy into a smaller buffer;
  - **compaction** on both backends, which is the one build-time flag neither implements.
- **A group's implicit constant buffer is one upload each.**
  `create_binding_group` allocates a buffer for a generated group's plain members and fills it with one copy per group.
  On `ctx.transient` that copy is recorded inline into the `cmd` the call takes, and on `ctx.persistent` it goes through `ctx.upload`.
  A per-frame group wants a transient constant-buffer writer instead: a ring in host-visible device memory (ReBAR where there is some), suballocated per epoch and written in place.
- **A resource's lifetime scope is stamped after construction, through a friend.**
  `raw_buffer::scope()` / `raw_texture::scope()` read a `mutable` field that `context_transient_scope` sets on the handle the backend just returned.
  Every backend already receives `allocation_info::scope` when it creates a resource, so each could forward it to the `raw_buffer` / `raw_texture` constructor instead.
  The field then becomes `const` and the friend goes away.
  It touches all eight backend resource classes and the four test fakes that construct the bases, metal's included, for no behavioural change.
  So it is its own change rather than part of the one that added the stamp.

- **The metal encoder-boundary barrier pair is emitted but not proved.**
  `flush_barriers` now emits on whichever encoder is open, clamped to the stages that encoder accepts, and every
  encoder publishes its work as it closes — so a dependency crossing an encoder boundary is carried by that publish
  plus the queue wait the next encoder opens with, rather than by the encoder-scoped barrier that cannot reach across
  one.
  That replaced the clamp's old "a raster dependency is the raster milestone's problem" comment, which had outlived
  its premise.
  What is *not* established is that the pair is load-bearing.
  `sg metal - a draw reads the vertex buffer a dispatch in the same list wrote` covers the path and passes with the
  publish removed, because a four-thread dispatch finishes well before the pass it precedes on an M4.
  A test that would catch the ordering needs a dispatch long enough to lose the race, which trades a sharp test for a
  slow one — so the pair stands on Apple's documented model rather than on an oracle of ours.
  The in-pass case is one step better off: a fragment-stage producer closes and reopens the pass, and
  `metal_command_list::pass_reopens` makes that observable, so `sg metal - a draw sees what the previous draw's
  fragment shader wrote` asserts the mechanism fired rather than only that the pixels came out right.
  The ordering itself is still the same race, and still not what a 4x4 draw can prove.

- **An odd first index into a 16-bit index buffer is refused everywhere, and closing that would need a shifted copy.**
  `sg::index_buffer_offset_alignment` is now a portable rule every backend asserts, so the failure is the same on all
  of them rather than metal-only — see [concepts/raster-pipeline.md](concepts/raster-pipeline.md).
  What it costs is real: a sub-mesh whose first index happens to be odd is a legal D3D12 and Vulkan draw that sg
  rejects, so a caller pads the range or uses 32-bit indices.
  Lifting it means staging a shifted copy of the index range, which needs a copy the render pass it sits inside cannot
  record — so it wants a pre-pass fixup or an aligned index allocator, and neither is worth building before something
  hits it.

- **The metal tier-2 tests block on `block_until_idle` where they could await `idle_completion()`.**
  `.shaped-lint.yml` allows that by name.
  Converting them was tried and reverted while `SC_THREADS=OFF` was still buildable on macOS, since it aborted there at the first download.
  That build no longer exists: `SC_THREADS=OFF` is refused on Apple targets, per [docs/platforms.md](../../../../docs/platforms.md#threading-sc_threads).
  So the conversion is unblocked whenever someone wants it.

- **No metal shader toolchain exists.**
  `sg::shader_format::metal_lib` implies one does, and nothing in the tree produces a metallib.
  `shaped-shader-library` has no metal arm, and the only metallibs are hand-compiled test fixtures checked in beside their `.metal` sources.
  So the metal backend's ray-tracing and compute paths are reachable by a caller who brings their own bytecode and by nobody else.
  The agreed shape for a fixture is HLSL run through SPIRV-Cross once by hand, with all three artifacts checked in.
  That matters because the argument-buffer layout was chosen to match what SPIRV-Cross emits, so a hand-written kernel would pin a convention no real pipeline produces.
  **What blocks it is DXC, and only on the host.**
  Microsoft ships no macOS release binary, and building it from source is an LLVM-scale build.
  [extern/dxc/dependency.yml](../../../../extern/dxc/dependency.yml) records that as `unavailable_on: [macos]`.
  SPIRV-Cross is not vendored at all, but it is plain CMake and would build here; it is not the constraint.

  That splits the work into two pieces with different costs, and they are worth deciding separately.
  Regenerating the *fixtures* needs DXC once, on any machine — the artifacts are checked in either way, so a Windows or Linux host does it and macOS never needs a compiler.
  A *toolchain* — a metal arm in `shaped-shader-library`, compiling at build time — is what genuinely needs DXC where the build runs, and there is no macOS path to HLSL → SPIR-V today.
  Apple's own `metal` command-line compiler is the third shape.
  It takes MSL rather than HLSL, so it would serve metallibs while giving up the one-source-two-backends property the HLSL route exists for.

  **The stand-in is `backends/metal/tests/raytrace.metal`**, hand-written and marked temporary in its own comment — regenerate it from HLSL once the toolchain exists.

- **Metal implements refit, compaction and placement natively, and sg exposes none of them.**
  Recorded here so the eventual surface is designed against three APIs rather than two.
  `MTL4::ComputeCommandEncoder` carries `refitAccelerationStructure(source, descriptor, destination, scratch)`, with `VertexData` / `PerPrimitiveData` options.
  Beside it are `copyAndCompactAccelerationStructure` and `writeCompactedAccelerationStructureSize`.
  `MTL::Device::accelerationStructureSizes` already returns the refit scratch, which is the number `update_scratch_size_in_bytes()` holds a slot for.
  Placement is `heapAccelerationStructureSizeAndAlign` plus `MTL::Heap::newAccelerationStructure(size, offset)`.
  All three APIs support refitting in place and into a separate structure, so a refit call would not be a Metal shape the others get bent into.

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
  It is load-bearing on the wasm builds without threads, where every one of those atomics keeps its interlock for a concurrency that cannot happen.
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
- **The VA-range allowlist entry is wider than the case it is for.**
  `resources contain the GPU Virtual Address range` in `dx12_expected_messages.hh` is a substring match, shared by every listener.
  So two persistent placements overlapping through a caller's allocator bug would be muted too, not only the transient heap's reuse.
  It stays unnarrowed on purpose: real applications raise the message many times per frame, and its text differs between drivers, so parsing each one is unattractive.
  No sg test provokes it yet; only sv's frame loop does.
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
  `try_acquire()` takes **no** lock where it wants a shared one, and `try_acquire_exclusive()` serializes `execute` calls that would be free to overlap.
  The clean-core extension it needs is a `cc::shared_mutex<T>` next to `cc::mutex<T>` — `lock_shared(f)` / `lock_shared_scoped()` alongside `lock(f)` / `lock_scoped()`.
  Then `try_acquire()` holds a shared guard for the caller's read and `try_acquire_exclusive()` keeps the exclusive one.
  The phases are a separate question: they are coroutines and can hold no lock at all across a suspend.
  What excludes them from an `execute` today is that they run only inside a tick, which is a frame-boundary call.
  See [render_routine.hh](../src/shaped-graphics/routine/render_routine.hh) and [render-routines.md](render-routines.md#threading).
- **Free-threaded resource creation.** Asyncs, layouts and samplers are free-threaded on every backend, and resource creation is still bound.
  Nothing forces that: a `main_thread` or `single_threaded` backend could record a buffer's or texture's description and make the object on first use, as webgpu's layouts do.
  An initial upload would then have to queue too.
  What it buys is concurrent scene loading against a thread-bound context without funnelling every create through its thread.
  See [concepts/threading.md](concepts/threading.md).
- **Thread model nuance:** `sg::thread_model` says which thread may make the bound calls.
  Grow it as needed — e.g. whether concurrent command-list recording is allowed, or per-queue guarantees.
- **Swapchain / presentation.** See [concepts/presentation.md](concepts/presentation.md).
  dx12, vulkan and metal are real, windowed and headless; webgpu presents headless and to a canvas.
  Still open:
  - **deeper HDR** — metadata and tone-mapping beyond the colorspace set.
    Including whether the request was *granted*: `enable_hdr` is best-effort on dx12 and vulkan, and webgpu's canvas has no HDR arm yet, and `is_hdr_enabled()`
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
  **The likely cause has since been found and fixed**, though not by chasing this.
  Two lists recorded concurrently against one resource each computed their barriers against the state it was in *while they recorded*.
  So neither saw the other's declares, and both took the no-barrier freebie.
  That is exactly this shape — a readback that needs the concurrency and sees nothing.
  Re-test it against the entry-barrier model before treating it as open.

- **Directly awaitable futures, if `co_await future.data()` ever reads as noise.**
  `bytes_future`, `data_future<T>` and `gpu_timestamp` hand out their result through `bytes()`, `data()` and `ticks()`, each a `cc::shared_async` built from `completion()`.
  An `operator co_await` on the future types would let a caller write `co_await future` instead.
  It was left out so sg's value types carry no coroutine machinery, and it is additive whenever it is wanted.

- **Readiness as an async, so the GPU tests stop working around it.**
  The suites are async: the entry drivers await `nx::async_invoke_tests_in_sequence`, and the tests await `ctx.idle_completion()`.
  Two workarounds remain, both waiting on a readiness signal rather than on the migration.
  A tick drives only routines already registered, so a test prewarms each variant by name before asserting on its first frame; a routine's readiness as an async would let it await instead.
  sv's `frames_until_executed` and the furnace loop re-record frames until a path-traced state object lands; the same signal for that would turn both into one await.
  What still blocks otherwise — manual window loops, fuzz steps, a few synchronous compile helpers — is allowed by file in each library's `.shaped-lint.yml`.

- **Tier 2 / legacy backends:** metal, then opengl, webgl.
  webgpu exists on wasm; what it still owes is its own item below.

- **There is no SGL to metallib edge, so the shader-using half of the tier-1 sweep skips on metal.**
  `shader_fixtures.cc` registers SGL to WGSL and, where DXC exists, to DXIL and SPIR-V.
  A metal context accepts none of those, so every tier-1 test that acquires a shader is offered a format it cannot
  take — eleven of them, across `compute-test.cc`, `raster-test.cc` and `sgl-package-test.cc`.
  They now ask `sg_test::shaders_reach` and SKIP rather than failing on an acquire that cannot succeed.
  **CI never saw this**: its macOS runner has no Metal 4 device, so the whole metal driver skips there, and the
  failure only appears on a Mac that has one.
  Closing it is an SGL-to-MSL compiler, at which point the guard answers true and the eleven start running with
  nothing to revert.
  `sg - the SGL fixtures reach at least one format on every build` is what keeps the guard from quietly skipping them
  on every backend instead.

- **The webgpu backend's remaining gaps.**
  - **The WGSL twins of sg's tier-1 shader tests.**
    The shader package and the routine tests need DXC, so on wasm no tier-1 test dispatches or draws.
    `shaped-graphics-webgpu-test` covers compute, raster, group 3 and presentation by hand until a WGSL package runs the same tests.
  - **The shaped-rendering blit and imgui shaders in WGSL**, which is what a routine on the web needs; the rotating-cube example already runs on webgpu.
  - **Per-test attribution of WebGPU errors.**
    One arriving after the test that caused it lands on the driver; an error scope per invocation would name the test.
  - **A stream whose source has nothing ready cannot be waited for** when a list touches its resource, so that list sees what landed so far and a warning.
  - **Storage views ignore `depth_slice_range`**, which WebGPU cannot express.
  - **emdawnwebgpu passes `WGPU_QUERY_SET_INDEX_UNDEFINED` to JS as 4294967295**, which wgpu refuses and Dawn accepts.
    Each query set's last slot is a discard target until that is fixed — docs/bugs-external/webgpu-timestamp-write-index-sentinel.
  - **A native Dawn build**, an additive CMake gate over the same sources.

- **`sv::viewer` still throttles by blocking, because its frame API is synchronous.**
  sg itself has no blocking spelling any more: frame loops await `epochs_in_flight_completion`, drains await `idle_completion`, and `ctx.execution()` is how a context says it cannot block at all.
  The rotating-cube and cube-editor examples and the sg, sr and sv window tests all await.
  `sv::viewer`'s pull loop (`is_running`, `end_frame`, its destructor) cannot, so it blocks on those completions with `cc::async_blocking_get`.
  That is a marked workaround, and it asserts on a `never_block` context; an async frame loop for `sv::viewer` is what retires it.
  The dxc end-to-end window tests block the same way, being synchronous throughout.
  No frame loop in the tree uses `try_advance_epoch` yet.
  The completion-signal seam is callback-shaped now (`arm_completion_signal`), and a never-block tier-1 driver on dx12 and vulkan proves the suite gets by without waiting.
  The gap that driver leaves is **dx12's and vulkan's own caller-thread waits**, which still wait under a `never_block` config.
  They are inline ring back-pressure, a ring budget change, the transient descriptor ring and swapchain acquire.
  All are backend internals no WebGPU code reaches, and the intended fix is the growth fallback *after* the wait, with a knob to skip the wait and trade VRAM for throughput.

- **`shaped-graphics-test` crashed once with an access violation, in the release preset under load.**
  The sixth of twelve loaded repeats of the release suite faulted; the binary has no symbols there and its log was overwritten before the faulting site was known.
  The crash hook named "sg - a routine whose init fails reports failed, not pending" as the only running test, and that test alone passed 400 repeats.
  The faulting thread's own stack stopped at the exception dispatcher, and one pool thread was mid-work in unsymbolized frames.
  Reproducing it wants a symbolized optimized build under the same load, and the log copied aside the moment a run fails.

- **Synchronization validation reported a `WRITE_AFTER_WRITE` between two transfer copies, once in about three loaded runs.**
  All 479 tests passed; the run failed on eight unattributed checks from the validation listener, each naming one transfer command buffer writing one `VkBuffer` twice.
  Two copies in one async window cannot happen: each window records one copy into its own of the three reused command buffers, so the same handle is a slot reused three windows later.
  Per-destination order holds, since `transfer_scheduler` only picks a family's head, and every window carries a memory barrier ahead of its copy.
  **The likely cause is the layer rather than the copies.**
  A window submit waits on other timelines, and the layer defers checking a submission whose waited value it thinks is unreached.
  It replays that check inside a later submit, which is the path `vulkan_epoch.cc` already avoids for host signals.
  **What is in place for the next occurrence:** both async transfer systems keep their last 32 windows.
  The sg vulkan driver appends to an `_AFTER_WRITE` failure the ones whose command buffer, destination or staging buffer the message names.
  It was seen again on 2026-09-21 under the full suite, as `WRITE_AFTER_READ` and `WRITE_AFTER_WRITE` on two buffers the stream test did not own.
  That dump was useless: it printed every upload window, all to one other buffer, and the log's size cap cut it off before the download windows.
  A system none of whose windows match now says so, which rules its copies out: the next dump either shows the two copies or clears both systems.
  If the two copies overlap and were queued out of submission order it is a real bug; if they are disjoint or in order it is the layer.
  The experiment beside it is dropping `wait_token` and `download_wait` from a window submit once the host counter shows them reached, which the comment at that wait chose not to do.
