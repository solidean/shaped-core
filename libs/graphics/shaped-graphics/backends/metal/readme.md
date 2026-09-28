# The metal backend

`sg::backend::metal` — shaped-graphics on Metal 4, for macOS and iOS.

Real across the surface, presenting windowed and headless and recording GPU timestamps.
The device, the queue, the epoch timelines, the command-list lifecycle, buffers, memory heaps, barriers, inline transfer and the bind path's layouts and groups are real.
Staging binding groups work too, which is what makes bindless arrays work — they are pure sg on top of one.
Compute pipelines build from a metallib and dispatch, textures create, bind and transfer, a swapchain presents, and raster draws — indexed and not, over vertex buffers, with inline constants.
Async transfer and streaming are real, and so is ray tracing — acceleration structures, a bound TLAS, and the DXR-shaped pipeline path.
[docs/writing-a-backend.md](../../docs/writing-a-backend.md) is the milestone order it is being filled in along.
[docs/concepts/backends.md](../../docs/concepts/backends.md) says what a backend is.

## The floor is Metal 4, and it is refused by name

`sg::create_metal_context` fails, rather than degrading, on anything below **macOS 26 / iOS 26 and a GPU in the Metal 4 family** — Apple silicon, M1 and A14 and newer.

Metal 4 is the generation that made barriers explicit, gave a command buffer its own allocator, and replaced `useResource` with residency sets.
Those three are not conveniences here: they are what lets sg's access model, its epoch-recycled allocators and its per-list touched-resource lists map across rather than be emulated.
On Metal 3 the driver hazard-tracks for you, so sg's computed barriers are either redundant or replaced by coarse `MTLFence`s, and there is no allocator to recycle at all.

A Metal 3 path would therefore be a second recording backend behind one context, exercised on nobody's machine.
That is the shape [writing-a-backend](../../docs/writing-a-backend.md) argues against for capability probes generally.
So the floor is stated once, checked before any `MTL4` selector is touched, and reported with a message naming the OS and the chip.

**MoltenVK is not an alternative to this backend**, and the reason is a hard requirement rather than a performance one.
sg's vulkan backend requires `VK_EXT_descriptor_buffer`, which its whole bind path is built on, and MoltenVK does not implement it — so that route does not start, rather than running slower on macOS.
`vulkan_swapchain.cc`'s refusal of the cocoa window platform records the same fact from the other side.

The OS check comes first on purpose.
This backend is compiled against the macOS 26 SDK, so on an older OS the MTL4 selectors do not exist and calling one is a crash rather than a diagnosable failure.

## It is C++, not Objective-C++

Every source here is an ordinary `.cc` over [metal-cpp](../../../../../extern/metal-cpp/dependency.yml), Apple's header-only C++ interface.

The deciding argument was our own tooling rather than the API.
`format.py`, `fingerprint.py`, `crossrefs.py` and the PCH tiers all hardcode `('.cc', '.hh')`.
A single `.mm` source would therefore mean editing the dev driver before writing a line of backend.
metal-cpp also wraps QuartzCore, so even `CAMetalLayer` — the one place presentation genuinely touches Cocoa — needs no Objective-C.

The pin tracks the installed SDK rather than the newest release: a wrapper newer than the framework on disk declares selectors that do not resolve.

## Where Metal diverges from the other two backends

Each of these is a fact about Metal rather than a gap in the backend.

- **There is no blit encoder.**
  MTL4's compute encoder carries `copyFromBuffer`, `copyFromTexture` and `fillBuffer` alongside dispatch, where dx12 and vulkan each have a distinct copy path.
  So one encoder serves both, and a barrier on it may name `MTLStageBlit` and `MTLStageDispatch` alike — but *only* those, plus `MTLStageAccelerationStructure`.
  `barrierAfterEncoderStages` refuses any other stage outright, which is why the translation clamps.
  **A barrier names stages, not resources.**
  `sg::pipeline_stage_flags` maps onto `MTLStages` directly: `vertex` to `MTLStageVertex`, `compute` to `MTLStageDispatch`, `copy` to `MTLStageBlit`.
  The resource list an sg barrier carries has nowhere to go.
- **Residency is declared, and nothing reports its absence.**
  MTL4 removed `useResource`: a resource outside every `MTLResidencySet` the queue knows about is simply not there when the GPU runs, so a copy from it reads zeroes and a copy to it writes nowhere.
  It is not an API misuse, so the validation layer says nothing either.
  One context-wide set today; a per-list set built from the touched-resource tracking is the optimization, not a correctness gap.
- **Queue barriers come in pairs, and the pair does not survive a queue wait.**
  `barrierAfterQueueStages` at the head of an encoder waits on queue work; `barrierAfterStages` at its end publishes this encoder's work to what follows.
  Both are unconditional per encoder, because emitting only the consumer leaves the wait with no producer to find.
  **But a `wait` on the queue between two commits resets what the later encoder's consumer half can see**, and a pending async transfer puts one there on almost every submit.
  So two command buffers are ordered by the submission timeline instead: a submit waits on the highest submission token any resource it touches was last named by.
  Per-resource, so lists sharing nothing still run concurrently.
  `tests/barrier/cross-list-ordering-test.cc` is the sequence that fails without it — and it passes on its own, without the transfer alongside, which is what made this invisible.
- **The epoch fence says the GPU is done, not that the host has read the bytes back.**
  An inline download's copy out of the staging ring runs from the commit's feedback handler, on a queue Apple schedules, so the fence can pass first.
  Reclaiming the span on the fence alone then hands those bytes to the next epoch while a download still owes them.
  Each of the download ring's epoch checkpoints therefore counts the copies outstanding against it, and reclaim stops at the first that has any.
  Same shape as dx12's download ring; the overflow buffer a large download gets instead is held by a retain of its own, released by the copy out rather than by the epoch.
- **A binding group is one argument buffer, and the layout is `binding.index` directly.**
  Metal has no descriptor-set layout object and no root signature, so both layout types here are schema and make no device call at all.
  A group becomes one argument buffer whose 8-byte slot `n` is what `[[id(n)]]` addresses in MSL.
  A buffer holds a GPU address, a texture or sampler an `MTLResourceID`, and an array binding takes `count` consecutive slots.
  That is what SPIRV-Cross emits for a descriptor set, which is the whole reason the layout was chosen.
  A group holds every resource it names: an argument buffer is raw addresses, so nothing else keeps the target alive.
  A staging group is the CPU-side image of one such buffer, and a snapshot is a fresh buffer copied from it — which is what makes snapshots independent of the builder and of each other.
  It needs **one** descriptor array where dx12 needs two: an argument buffer has no view/sampler heap split, so both of the base's offsets are `binding.index`.
  **An array binding consumes `count` indices, and nothing checks that.**
  Unique indices are not enough on their own: an array at index 0 with count 4 occupies slots 0 to 3, so a scalar binding at index 1 overwrites one of them and no error says so.
  **A layout with array bindings must space its indices by their counts**, which is a rule rather than a check.
  A layout is built often enough that a pairwise range test there costs every correct layout — see [docs/concepts/bindings.md](../../docs/concepts/bindings.md).
- **There is no per-pipeline cached blob.**
  `cached_pipeline_data()` returns empty and `used_cached_pipeline()` is always false, so every pipeline build is cold.
  Metal 4's `MTL4Archive` is one store per *compiler* where sg's surface is one blob per *pipeline*, so the mapping
  vulkan found — a `VkPipelineCache` per pipeline — has no counterpart here.
  Recorded as open in [docs/TODO.md](../../docs/TODO.md), and pinned by a test so it reads as deliberate.
- **Vertex buffers and inline constants are argument-table addresses, because Metal has neither of the usual forms.**
  MTL4's render encoder has no `setVertexBuffer`, and Metal has no root constants and no push constants — so both arrive the way a binding group does, as an address in the one `MTL4ArgumentTable`.
  That fixes a buffer-index convention the shader has to agree with, and `metal_common.hh` is where it is stated.
  Groups at 0 to 2, sg's reserved group at 3, inline constants at 4, and vertex-input slot `n` at 5 + `n`.
  An `[[attribute(n)]]` index is the attribute's position in `vertex_input_layout::attributes`, which is the workaround the vulkan backend already states for its SPIR-V locations.
  Both are a workaround for the same missing field: sg names a vertex input by an HLSL semantic, and neither MSL nor SPIR-V has one.
  **Nothing below checks any of it.**
  A wrong buffer index is not an API error here; it is a draw that reads whatever else was bound there, which is why `tests/metal-vertex-input-test.cc` exists.
- **Inline constants are staged by the draw, not by the setter** — the shape WebGPU's backend arrived at for the same missing feature.
  A set patches a host-side shadow, and the next dispatch or draw copies the block into the upload ring and binds its address.
  An unchanged block is bound again at the address it already has, so a list setting the same constants for every draw stages one block rather than one per draw.
  A *changed* one takes a fresh span rather than a rewrite in place, because the address an earlier draw was recorded against is still what that draw will read.
- **An index buffer must sit at a 4-byte boundary, and nothing says so when it does not.**
  MTL4's `drawIndexedPrimitives` takes the indices as a GPU address with no first-index of its own, so sg's `index_range.offset` is folded into that address.
  An odd first index into a `uint16` buffer therefore lands 2 mod 4.
  Metal then draws part of the mesh and reports nothing: not an error, not a validation message, just a partly-drawn mesh that D3D12 and Vulkan both draw whole.
  It was found by a test whose quad came out as one triangle, which is the only way it can be found.
  **This backend is the reason `sg::index_buffer_offset_alignment` exists**, and the rule is sg-wide rather than metal's.
  Every backend asserts it, so the violation fails on whichever dev box the author has.
  [concepts/raster-pipeline.md](../../docs/concepts/raster-pipeline.md) is the rule, and `sg::is_aligned_index_fetch` answers it without asserting.
- **A render encoder's barrier is asymmetric, where a compute encoder's is not.**
  `barrierAfterEncoderStages` on a render encoder refuses `MTLStageFragment` as its *source* by name, accepting only `MTLStageVertex | MTLStageObject | MTLStageMesh`.
  Inside one draw the fragment stage is last, so there is no later stage of that draw for work ordered after it to reach.
  Its destination half takes the whole pass.
  Hence two clamps rather than one, and the validation layer aborting on the pair is how the asymmetry was found rather than read.
- **A fragment-stage producer is ordered by closing and reopening the pass.**
  The reasoning above holds for one draw and not for two.
  A fragment shader writing what a later draw in the same pass reads is a dependency no barrier here can name, and one clamped to the vertex stage orders nothing that matters.
  So `flush_barriers` ends the render encoder and opens it again over the same targets, with every load op forced to LOAD — the same answer vulkan gives.
  The encoder boundary's publish/wait pair is what carries the dependency.
  The scope's encoder state is replayed onto the new encoder, since none of it survives the boundary.
  `metal_command_list::pass_reopens` counts them, which is what the tier-2 test asserts rather than trusting the pixels.
- **Every encoder publishes as it closes, rather than the list publishing once at the end.**
  An encoder-scoped barrier orders work inside its own encoder and cannot reach across a boundary, so a dispatch written and then read by a draw is ordered by the pair at that boundary instead.
  That pair is the publish `end_encoder` and `raster_end_rendering` emit, plus the queue wait the next encoder opens with.
  That is also what makes a mask clamping to nothing in `flush_barriers` harmless rather than a dropped dependency.
- **An array binding's elements are declared one at a time, and an undeclared one is an error.**
  Which elements a shader indexes is decided by data no backend sees, so a group keeps its array bindings apart from its scalar ones.
  The scalar bindings are declared automatically at the dispatch or draw, and the arrays only by `cmd.compute.declare_array_*_access`.
  A bound array binding with no declare asserts rather than going untracked, and an empty span is how a caller says one is unused — the same accounting dx12 and vulkan keep.
  An acceleration-structure array is the exception and stays automatic.
  A trace reads every structure its table can reach, and sg's two declare calls are split by buffer and texture with no third for that kind to arrive through.
- **A raster pipeline is three objects where dx12 and vulkan have one.**
  MTL4 splits what a D3D12 PSO folds together.
  The render pipeline state carries the shaders, the vertex layout and the colour attachments' blending.
  The depth and stencil test is a separate `MTLDepthStencilState` bound on the encoder.
  Cull mode, fill mode, winding, depth bias and depth clip are encoder calls rather than pipeline state at all.
  The third group is replayed on every `bind_pipeline`, which is what keeps it consistent with the pipeline a caller believes is bound.
- **A depth-only pass keeps rasterization on.**
  A pipeline with no fragment function is a depth-only pass, and Metal runs one from the vertex stage alone.
  `setRasterizationEnabled(false)` because there is no fragment function discards every primitive *before* the depth test, so the pass writes nothing at all.
- **One sg depth-stencil target is two Metal attachments.**
  A combined format needs `stencilAttachment` set from the same texture as `depthAttachment`, with its own load/store action and `setClearStencil`.
  Without it the stencil is never cleared and every stencil test compares against whatever was in memory.
- **Attachment formats are not pipeline state.**
  MTL4's render pipeline descriptor has no depth or stencil attachment format at all, and its colour formats are only what the blend descriptor needs.
  The render pass establishes the rest at encode time.
  sg's `depth_stencil_format` is therefore carried for validation rather than for building — `metal_raster_pipeline::depth_stencil_format()` is what the rendering scope is checked against.
  That is the opposite of dx12's DSVFormat and vulkan's dynamic-rendering formats.
- **A barrier is flushed before the render encoder opens, not inside it.**
  Vulkan forbids a barrier inside a dynamic-rendering instance and closes the pass around one.
  Here the declares are simply flushed first, which costs nothing a frame that transitions its targets up front was not already paying.
- **A pipeline is built through an explicit compiler object.**
  MTL4 makes compilation an `MTL4Compiler` the context owns, where Metal 3 hid it behind the device.
  A metallib blob reaches it as `dispatch_data`, and the entry point is named through an `MTL4LibraryFunctionDescriptor` rather than looked up on the library.
- **Bindings reach a dispatch through an argument table, not per-encoder setters.**
  One `MTL4ArgumentTable` serves a command list, and a group bound at slot N writes its argument buffer's address into buffer-binding N — so sg's `group_index` *is* the MSL `[[buffer(N)]]` index.
- **Textures have no layouts, so one access tracker serves both resource kinds.**
  `sg::texture_layout` has a D3D12 spelling and a Vulkan one and no Metal one, so a transition carries no layout half.
  What survives of a barrier is the stage and cache half, which MTL4 spells `barrierAfterEncoderStages:beforeEncoderStages:visibilityOptions:`.
  dx12 and vulkan each need two — a texture's tracker carries its layout and partitions it by subresource — and here a
  texture has no state a buffer does not also have.
  `metal_resource_access` is that one type, and `current_texture_layout` answers `general` for every texture and range
  because that is true rather than a placeholder.
  `cmd.ensure_layout` is honoured as a no-op rather than asserting, so portable code may call it unconditionally, which
  is what it is for.
  The subresource partition is an optimization not taken: a texture is one undivided state, so two mips written and
  read in turn get a barrier they would not strictly need.
- **A texture view is cached on a per-texture identity stamp, never on the texture's address.**
  That distinction is the vulkan build-out's most expensive bug repeated cheaply: a per-frame texture's address is
  recycled, so an address-keyed cache hands a new texture the previous one's view, of an object that no longer exists.
  It needs an allocator to reuse an address, so a suite reports it as flaky and a frame loop reports it every few
  seconds.
  **`hash(sg::raw_texture_view)` is not that identity**, and reaching for it was how this backend shipped the bug anyway: sg's view hash folds `texture.get()`.
  So the key is `metal_texture_view_key`, over `metal_texture::identity()` plus the fields that reach `newTextureView`, and each entry is evicted on its texture's own finalizer.
  The eviction matters on its own: a view retains its parent, so an entry that outlives the texture keeps that MTLTexture alive for the context's whole lifetime.
- **A view's MTLTextureType comes from the view's dimension, not the texture's.**
  A one-face view of a cube is a 2D texture; reusing the texture's own type asks Metal for a one-slice Cube, which it refuses.
  The whole-texture shortcut — return the texture rather than mint a view — therefore checks the type as well as the format and the range.
  And a cube's range counts faces where `arrayLength` counts cubes, so that range check multiplies by six.
- **A streamed texture row is a row of BLOCKS, and the extent is slice-major.**
  `staging_layout_of` counts rows in blocks, so a BC1 row covers four texel rows — treating the two as one fills the top quarter of a compressed texture and overruns a 3D one.
  And a chunk that crosses `bytes_per_image` continues on the next z, which one copy of depth 1 cannot express.
  So a chunk is encoded as one copy per slice, with the last block row of each clamped to the region's height.
- **A queue wait is FIFO, and that is what makes a stream waiting for a command list a cycle.**
  `MTL4CommandQueue::wait` is an ordinary queue item: work committed before it is unaffected — measured, not assumed.
  What it does gate is everything committed *after*, which is the problem: a stream spans several batches, so its later batches are committed behind another stream's wait.
  A list waiting on stream J and also touching stream K's resource has K's wait park the queue on it, and J's remaining chunks then queue behind that park.
  Metal breaks the tie after about four seconds and runs the two in the wrong order, which is a hundredfold stall and stale bytes rather than a clean stop.
  So the stream actor holds a job back until the host sees its `direct_wait` submission, and arms a `notifyListener` on the submission timeline to wake itself.
  It has to arm one: the actor sleeps whenever a cycle stages nothing, and a gated job has no source waker and no batch in flight.
  A listener of its own, because the completion waiter's armed values live without a lock precisely because it is that listener's only caller.
  `sg stream - two streams and a list that touches both do not deadlock` is the shape, and it fails on the queue-wait version.
- **A device refusal is an error here, never an assert.**
  Metal answers a refused allocation with nil rather than a status, which reads like a contract violation and is not one.
  [docs/error-handling.md](../../../../../docs/error-handling.md) routes it to a `cc::result` instead.
  So context creation, the queues, the residency set and the staging rings are fallible, and a refused sampler or texture view comes back null for the group being built to report.
  An off-frame transfer settles its future as cancelled, or reports on the deferred channel where it has no future — an upload has no return value to fail.
  Two seams throw instead, which is what that doc reserves an exception for: the argument table and a streaming timeline, both reached from recording calls whose caller can do nothing with an error.
- **Host-visible memory is free.**
  `MTLStorageModeShared` on unified memory is exactly the thing whose absence blocked every one of the vulkan backend's transfer paths.
- **A heap reports a size that is not a multiple of its own alignment.**
  `heapBufferSizeAndAlign` answers the two questions independently, and for a 1-byte buffer returns a size of 1 at an alignment of 256.
  D3D12 and Vulkan both round for you.
  `sg::context_transient_scope`'s bump allocator advances its head by the reported size and never re-aligns, so the backend rounds before reporting.
  Without that, every placement after the first lands unaligned.
  `sg metal - a heap's buffer requirements keep a bump allocator aligned` pins it.
- **A staging ring needs a tail, not a reset.**
  Rewinding the head when an epoch retires is the obvious shape and is wrong.
  Reservations made after that epoch closed already sit past the rewind point, so the bytes get handed out twice and the older transfer's data is overwritten before its copy runs.
  The ring records where the head stood at each advance and moves a tail there instead.
- **A download's copy-out runs on the commit, not on the epoch.**
  `block_until_idle()` drains the GPU without advancing, so an open epoch's payload never runs — a download deferred onto the epoch would stay unsettled forever in an unadvanced frame.
  `MTL4CommitFeedback` fires at exactly the right moment, and `block_until_transfers_drained` is sg's hook for waiting on the outstanding ones.
  That is why there is no readback actor here where the other two backends have one.
- **Placement works for textures from the start.**
  A Metal placement heap is not told what it will hold, so there is no buffers-only stage to grow out of the way dx12 has one.
- **Two queues need three waits, not one.**
  An off-frame transfer runs on a second `MTL4CommandQueue`, and neither queue knows anything about the other's timeline.
  A command list waits on `metal_transfer_system::pending_value_for` before it runs.
  A transfer waits on `metal_buffer::last_used_submission` before it copies.
  And a transfer waits on the same buffer's own previous transfer, which is the one that looks redundant and is not.
  Two commits on one queue are ordered, but the copies inside them are not — so an async download reads back what the async upload before it has not finished writing.
  Each direction has a tier-1 test of its own, and each of them passes with the other two waits in place.
- **A backend runs a resource's finalizers itself.**
  `raw_buffer::add_finalizer` puts them in a protected member and names the contract — released storage *and* a retired epoch — but nothing in sg core ever calls them.
  A backend that reclaims the GPU object and forgets the finalizers looks completely correct until a test asserts on one, which is what `sg - async upload to a dropped buffer still releases it` does.
  They run inside the epoch's deferred callback, after the Metal object is released: a finalizer reclaiming the memory a placed resource sits on must never observe a live handle into it.
- **A Metal callback is a real thread, and it is why this backend refuses `SC_THREADS=OFF`.**
  Every commit's `MTL4CommitFeedback` handler runs on a dispatch queue Apple owns, and no build flag of ours reaches it.
  `SC_THREADS=OFF` used to compile `cc::mutex`'s lock away underneath that.
  The residency set caught it immediately and fatally: `residency sets do not support concurrent write operations`, aborting the singlethreaded suite on the first async upload.
  The rule is now at configure time instead: an Apple target refuses `SC_THREADS=OFF` outright, per [docs/platforms.md](../../../../../docs/platforms.md#threading-sc_threads).
  So a build where `cc::mutex` is hollow and a Metal thread is live cannot be produced at all.
  A `callback_mutex` used to stand in for `cc::mutex` wherever a handler wrote, and two commit-handler counters were `std::atomic` for the same reason.
  Both are gone: `cc::mutex` and `cc::atomic` are the std types in every build this backend compiles in, so the stand-ins said nothing the originals did not.
  What survives is `std::mutex` and `std::condition_variable` in `completion_signal`, and that is a different reason — clean-core has no condition variable.
- **An async texture transfer needs no layout settling, where dx12 needs a whole command list for it.**
  A D3D12 copy queue cannot run layout barriers, so dx12 submits a direct-queue fixup before it stamps the job.
  Metal textures have no layout at all, so the off-frame path is the buffer path with a footprint: same queue, same two waits, `staging_layout_of` in place of a byte count.
- **Concurrent pipeline compilation aborts inside the driver.**
  Two threads in `MTL4Compiler::newRenderPipelineState` — on *different* compilers, from different contexts — abort in `_os_unfair_lock_corruption_abort` under `AGXG16GFamilyCompiler`.
  About one run in five on an M4 under macOS 26, and it is the driver rather than the validation layer: it reproduces with `MTL_DEBUG_LAYER=0`.
  So every pipeline build is taken under `pipeline_compilation_lock()`, process-wide rather than per context.
  The state being corrupted is the device's, and a Mac hands the same device to everyone who asks.
  `sg metal - pipelines build concurrently from several contexts` is the gate, and it is probabilistic: without the lock it takes the binary down within a run or two.
- **Off-frame transfer needs three queues, and each one is correctness rather than tuning.**
  A wait blocks everything committed after it on that queue.
  So an async transfer ordering behind an in-flight stream blocks the stream's own copies too when they share a queue, and the stream never finishes and the wait never clears.
  **Uploads and downloads are split for a second reason, found the hard way.**
  Sharing one queue reproduced a transfer race in the tier-1 fuzz about one run in four: an async download came back all zeroes while the buffer itself was correct.
  Splitting them took that to zero in sixty-five runs.
  It is the shape vulkan already has, and dx12 needs neither split — a D3D12 copy queue runs its command lists serially where an MTL4 queue is concurrent by default.
  **Each queue needs a timeline of its own.**
  One shared event cannot take signals from two queues: they complete independently, so a later value can land first, drive the event backwards and release a waiter early.
- **A streaming transfer needs a timeline per resource, which the async tier does not.**
  A list touching a streamed resource waits for the *whole* transfer, including chunks the actor has not staged yet — so the value is reserved when the transfer is admitted and signalled when it ends.
  On one shared timeline that is unsound: transfers finish out of order, and a later one signalling its value would report an earlier one complete.
  Per resource it is sound, because sg runs two transfers of one resource in submission order.
  The event is signalled by the CPU rather than the queue, since what it reports is a job ending — which may be a cancellation with no GPU work at all.
- **A streaming transfer's direct-queue wait is read once, at admission, and reading it per chunk deadlocks.**
  A list touching a streamed resource waits for the whole transfer, so a chunk staged after that list was submitted would wait for a list waiting for it.
  Admission is also what the contract says: the extent is the caller's alone from the call onward, so a list submitted later has no claim to order ahead of the stream.
- **A GPU fence is not always outside the pump registry.**
  clean-core's `thread_pump.hh` says blocking on a GPU fence is fine because nothing registered has to run for it to be signalled — and the streaming tier breaks that premise.
  A list waiting on a streaming transfer reaches its fence only once the actor signals it, and in an unthreaded build that actor runs on whoever waits.
  So every wait in this backend pumps, and the singlethreaded preset is where forgetting it shows up as a hang rather than as a slow test.
- **Metal orders the async tier against in-flight streams, where dx12 does not.**
  The per-resource streaming timeline is what makes it cheap: an async transfer waits on the same value a command list would.
  `promote_to_async` is then purely the statement of intent it is documented to be — it suppresses the warning, and adds no wait, because the wait is already there.
  docs/TODO.md records the gap on the backend that still has it.
- **An acceleration structure is not a buffer.**
  DXR names one by the GPU virtual address of the buffer the driver built it into, and Vulkan wraps an object around such a buffer.
  `MTL::AccelerationStructure` derives from `MTL::Resource` and there is no `MTLBuffer` in the caller's hands at any point.
  So `sg::blas` and `sg::tlas` carry no storage handle at all.
  The one they used to carry was a D3D12 fact that had leaked into the portable layer.
- **There is no software device.**
  dx12 has WARP and metal has nothing, so coverage here is developer-machine-only and a host below the floor makes every test `SKIP`.

## Ray tracing: both paths, and a raygen shader that is the kernel

**There is no MTL4 ray-tracing pipeline.**
DXR hands the driver a set of shaders and lets it schedule them.
Metal dispatches an ordinary compute kernel that calls `intersector` itself, with function tables supplying what traversal and the kernel call back into.
So a raygen shader is not something a pipeline dispatches — it **is** the kernel.

Both of sg's paths are real here.
Inline ray query needed nothing but the bind path.
`sg::tlas::as_view()` was already portable, and a TLAS binds by `gpuResourceID()` into an argument-buffer slot exactly as a texture does.
The pipeline path maps as follows.

- **One MTL4 compute pipeline per registered raygen shader**, each dynamically linked with every hit, miss and callable function the description registered.
- **`raytracing_shader_table` becomes four Metal tables**, and that is a language constraint rather than a preference.
  MSL's `visible_function_table<T>` is typed by the function signature, so one table cannot hold miss, closest-hit and callable functions.
  Their signatures differ, and the compiler rejects calling one table two ways.
  Each of sg's index spaces therefore gets a table of its own, and `miss_index` / `hit_index` / `callable_index` are used verbatim with no base to add.
- **The tables reach a kernel through `sg::reserved_binding_group`**, as four members of that group's argument buffer:
  `[[id(0)]]` intersection, `[[id(1)]]` miss, `[[id(2)]]` closest-hit, `[[id(3)]]` callable.
  The reservation already existed for exactly this, so nothing about what `group_index` means changes.
  A caller still gets groups 0 to 2.
  Ray tracing and shader-side diagnostics now share that group, so those `[[id(n)]]` assignments are one namespace rather than two.
- **One table set per raygen**, because a function handle is minted from a specific pipeline state and this pipeline has one state per raygen shader.
- **One `MTL::Library` per registered shader**, so a table may draw its entries from as many separate shader files as it has entries.
  That is the realistic shape rather than a nicety: `sg::compiled_shader` is single-entry, so a real shader pipeline hands the backend one blob per shader.
  `sg metal - a shader table is built from two separate libraries` pins it, with the miss function coming from a second metallib whose sentinel value is what says which library ran.
- **An sg hit group splits across both kinds of table** at the same index.
  `intersection` and `any_hit` run *during* traversal and belong in the intersection function table.
  `closest_hit` runs after it and is called by the kernel, so it is a visible function like a miss shader.
  A triangle group with neither gets `setOpaqueTriangleIntersectionFunction`, asked for with `Instancing | TriangleData`.
  The signature must match the table's own MSL declaration, so **a kernel whose intersection table holds any triangle group declares it `intersection_function_table<instancing, triangle_data>`**.
- **A procedural hit group may not carry an any-hit, and metal refuses the pair.**
  Traversal runs exactly one function per group here, and for a procedural group that is its intersection function — there is nowhere to put an any-hit beside it.
  DXR runs both, so this is a real gap rather than a spelling: fold the any-hit's decision into the intersection function, which is already deciding what the ray hit.
  Accepting the group and dropping the any-hit is what this replaces, and it reports hits DXR would have rejected without saying anything.
- **Two of DXR's three hit-index contributions map.**
  The instance contribution is the instance descriptor's `intersectionFunctionTableOffset`, and the geometry contribution is each geometry descriptor's own offset, set to its geometry index.
  The ray contribution — DXR's per-`TraceRay` term — has no counterpart, because an MSL kernel names the table it calls.
  [docs/concepts/raytracing-pipeline.md](../../docs/concepts/raytracing-pipeline.md) carries what that costs a ported shader.
- **Dynamic linking rather than static.**
  `raytracing_pipeline_description` already owns every shader, so static would fit.
  It would also drop the property the handle-to-index split exists for: one pipeline backing several tables with different function sets.
- **`max_recursion_depth` maps onto `PipelineStageDynamicLinkingDescriptor::setMaxCallStackDepth`, and recursion works.**
  Apple's documentation for that property says to "change its value if you use recursive functions in your compute pass".
  It covers indirect calls — visible functions, intersection functions and dynamic libraries.
  **It defaults to 1**, so a backend that ignored the field would under-declare the stack rather than report anything.
  The units differ from DXR's, and the mapping is the conservative direction.
  DXR counts `TraceRay` nesting and this counts indirect-call nesting, and a recursive trace ported here spends at least one indirect call per level.
  `sg metal - a hit function recurses through its own table to the declared depth` pins it, recursing four levels through a self-referential visible function table.
  **The one place recursion genuinely cannot go is inside traversal.**
  An intersection or any-hit function cannot even take an `instance_acceleration_structure` parameter, which the compiler refuses by name.
- **`dispatch_rays` picks the threadgroup shape**, because sg's call carries none — it is a ray grid.
  The shape comes from the pipeline's `threadExecutionWidth` and `maxTotalThreadsPerThreadgroup`.

**Every intersection kind is exercised end to end**, by four tests that each dispatch two threads.
One thread aims at the geometry and one aims past it, so the hit path and the miss path are covered together and cannot be confused.
A hit reports its distance, a miss reports −1, and a payload nothing wrote stays 0: three distinguishable numbers, so "the wrong function ran" and "no function ran" are different failures.

| test | what it proves |
|---|---|
| an inline ray query hits and misses | the portable shape: traversal finds the triangle, and an empty direction reports a miss |
| dispatch_rays reaches the closest-hit and miss functions | the kernel called both through its visible function tables |
| an any-hit function rejects a hit during traversal | the any-hit ran: same geometry and same ray as the row above, now non-opaque, reporting a miss |
| an intersection function describes a procedural primitive | the intersection function produced the distance, which is not the AABB's own entry distance |
| a hit function recurses through its own table to the declared depth | indirect recursion works, four levels deep |
| a shader table is built from two separate libraries | table entries may come from different shader files |
| each geometry of a BLAS selects its own hit group | the geometry contribution reaches the intersection table |
| a procedural hit group with an any-hit is refused | the pair metal has no traversal slot for is an error, not a dropped shader |

The any-hit test is the pair of the one above it rather than a standalone assertion.
Opaque geometry reports a hit and the identical non-opaque geometry with a rejecting any-hit reports a miss, so nothing but the function could have changed the answer.
That matters because traversal consults no any-hit function on opaque geometry at all.

Writing them found real defects that compiled cleanly.
`dispatch_rays` never set the argument table on its encoder, and its threadgroup shape ignored the grid — which Metal rejects rather than trims.

**What sg cannot check, and neither can DXR.**
On DXR the driver invokes closest-hit and miss; here the kernel must.
One HLSL raygen plus separate miss and closest-hit entry points becomes one MSL kernel containing the traversal loop and the calls.
Table *indices* are no less checked than on DXR.
There `TraceRay`'s `MissShaderIndex` and hit-group offsets are equally raw indices into a table sg built.

What *is* checked, since Metal has no validation-message callback to lean on:

- a `functionHandle` that comes back null — an un-linked or misspelled function — fails the table build rather than becoming a wrong call at trace time;
- handle ranges are bounds-checked against the pipeline.

**It does not make `sv` run on macOS.**
`sv`'s path tracer is written against the DXR pipeline path and its shaders are HLSL, and there is no macOS route from HLSL to MSL.
MSL itself compiles — `shaped-shader-compiler-msl` — so porting those shaders means rewriting them in SGL or building an HLSL route elsewhere.
The backend having ray tracing and the viewer working on macOS are separate milestones.

## Validation: no callback exists, so the gate is an abort

This is the sharpest difference from dx12 and vulkan.
Both install a callback that fails whichever test provoked a validation message.
[writing-a-backend](../../docs/writing-a-backend.md) puts wiring one up second on its list of three things to do before any rendering code.
Metal has no such callback, and the design below follows from measuring what it does have rather than from any documentation.

What was measured, on macOS 26.4 with a zero-length `newBuffer` as the provocation:

| mechanism | what it delivered |
|---|---|
| `MTLLogState::addLogHandler` | **nothing** — 0 calls, layer on or off |
| `MTL_DEBUG_LAYER=1` | the message, on stderr via NSLog, and to nothing else |
| `MTL_DEBUG_LAYER_ERROR_MODE=assert` | a failed assertion, so the process aborts |
| `MTL4CommitFeedback` | fires per commit, carrying an `NSError` or none |

`MTLLogState` is the obvious candidate and is not the channel: it carries shader `os_log` output and the framework's own log, not the validation layer.
`sg metal - an MTLLogState handler is not the validation channel` pins that, so if Apple ever changes it we find out.

So there is no `set_message_callback` here and `metal_config` carries no validation flag — there is nothing for either to deliver.
**The gate is the abort instead.**
`arm_validation_layer()` sets `MTL_DEBUG_LAYER=1` and `MTL_DEBUG_LAYER_ERROR_MODE=assert`, and both test binaries call it from `main` before anything touches Metal.
That is the only moment that works: the framework reads those variables when it first initializes, and there is no API for them.
Neither variable is overwritten when already set, so a shell can pick a different mode.

The trade is attribution.
dx12 and vulkan fail one test and carry on; a metal violation ends the binary, and the log names the test that was running.
That is coarser, and it is a real oracle rather than none.
The alternative is the failure mode [testing](../../docs/testing.md) records, where dx12 accumulated roughly 680 unnoticed messages with the suite green throughout.

**The gate is proved rather than assumed.**
`sg metal - the validation gate aborts on a violation` is `nx::config::disabled`, because passing it means ending the process.
Run it by name and read the abort as the pass; a run that finishes means the layer is not armed.

**Parsing stderr is not an alternative.**
It would mean deriving a correctness signal from message text nobody controls, and redirecting a descriptor Metal writes to from its own threads.
A miss would then be silent, which is the same failure as having no oracle at all — and the abort's whole value is that it cannot be missed.

### Commit feedback is the one programmatic channel

`MTL4CommitFeedback` reports a failure that arrives after the call that caused it, which is exactly what sg's deferred error channel is for.
So every commit carries a handler, and an error reaches `ctx.take_pending_errors()`.

Metal's timeout, device-removed and access-revoked codes mean the device itself is gone and mark the context lost; everything else is one command buffer failing.

The handler runs on a dispatch queue at a time nothing here controls, which can be after `shutdown` has returned.
So it captures a [`metal_feedback_sink`](src/shaped-graphics/backends/metal/metal_feedback.hh) rather than the context, and shutdown detaches it.
A handler still in flight then does nothing instead of reporting into freed memory.

## Testing

`shaped-graphics-metal-test` is the tier-2 binary: bring-up, the floor refusal, the epoch timelines, command-list lifetime, transfer, the bind path, compute, raster, and what Metal reports where.

The tier-1 suite has **no compute execution test at all** — it cannot, because bytecode is per-backend by construction — so this tier is the specification for the dispatch path.
`double_compute.metal` is checked in beside the `double_compute.metallib.h` compiled from it, with the command line in the source's own comment.
`mesh.metal` is the vertex-input fixture, one library with five entry points.
The two stages of a draw that reads its colour from a vertex attribute and its tint from inline constants, plus three kernels.
One kernel takes inline constants, one takes an array binding, and one writes the vertex buffer a draw then reads.
Its quad carries decoy vertices and decoy indices ahead of the real ones, so a draw that ignored `vertex_offset` or the first index covers a different part of the target rather than the same one.
`raytrace.metal` is the ray-tracing fixture, one library with seven entry points.
Two raygen kernels and the inline ray-query one, plus a miss function, a closest-hit function, an any-hit function and a procedural intersection function.
It is what makes the two execution tests the only thing anywhere that traces a ray on metal, since the tier-1 suite cannot.

**That fixture is hand-written, and temporary.**
The agreed shape is HLSL run through SPIRV-Cross with all three artifacts checked in.
The argument-buffer layout this backend encodes was chosen to match what SPIRV-Cross emits, so a hand-written kernel can agree with the backend and still disagree with every real shader.
Neither DXC nor SPIRV-Cross is available on an arm64 macOS host today, which is why it exists in the meantime.
`raytrace.metal`'s own comment says so, and [docs/TODO.md](../../docs/TODO.md) carries the gap.
So the fixture is reproducible by hand, and the binary needs neither the shader library nor the Metal toolchain.
Its reflection is written out by hand next to it, so the test states the binding shape it means rather than inheriting whatever a reflector produced.
Every test builds its own context, because the context is its subject.
The whole binary runs with API validation armed, so a violation anywhere in it ends the run.

**The tier-1 API suite runs against metal**, 162 tests of it, through `tests/backends/metal-entry.cc` in the same shape vulkan's driver uses.

It was disabled and written synchronously until the review caught it, which meant nothing ran the sweep — the suite was green only for tests named one at a time.
Turning it on immediately found the cross-list ordering defect above, which no single test could reach, and that is the argument for enabling a sweep before it is comfortable rather than after.

The driver is unconditional now.
It was gated on `CC_HAS_THREADS` while an unthreaded Apple build was still possible; refusing that build at configure time removed the gate rather than the coverage.

One API test still runs against metal by being named exactly, which is what the per-invocable aliases are for:

```bash
uv run dev.py test "sg - advances an epoch"
```
