# The metal backend

`sg::backend::metal` — shaped-graphics on Metal 4, for macOS and iOS.

Early stage.
The device, the queue, the epoch timelines, the command-list lifecycle, buffers, memory heaps, barriers, inline transfer and the bind path's layouts and groups are real.
Staging binding groups work too, which is what makes bindless arrays work — they are pure sg on top of one.
Compute pipelines build from a metallib and dispatch, textures create, bind and transfer, raster draws, and a swapchain presents.
Async transfer and streaming still assert.
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

- **Textures have no layouts.**
  `sg::texture_layout` has a D3D12 spelling and a Vulkan one and no Metal one, so a transition carries no layout half and `current_texture_layout` answers `general` always.
  What survives of a barrier is the stage and cache half, which MTL4 spells `barrierAfterEncoderStages:beforeEncoderStages:visibilityOptions:`.
- **There is no blit encoder.**
  MTL4's compute encoder carries `copyFromBuffer`, `copyFromTexture` and `fillBuffer` alongside dispatch, where dx12 and vulkan each have a distinct copy path.
  So one encoder serves both, and a barrier on it may name `MTLStageBlit` and `MTLStageDispatch` alike — but *only* those, plus `MTLStageAccelerationStructure`.
  `barrierAfterEncoderStages` refuses any other stage outright, which is why the translation clamps.
- **Residency is declared, and nothing reports its absence.**
  MTL4 removed `useResource`: a resource outside every `MTLResidencySet` the queue knows about is simply not there when the GPU runs, so a copy from it reads zeroes and a copy to it writes nowhere.
  It is not an API misuse, so the validation layer says nothing either.
  One context-wide set today; a per-list set built from the touched-resource tracking is the optimization, not a correctness gap.
- **Queue barriers come in pairs, and one half alone synchronizes nothing.**
  `barrierAfterQueueStages` at the head of an encoder waits on queue work; `barrierAfterStages` at its end publishes this encoder's work to what follows.
  Emitting only the consumer leaves the wait with no producer to find, and a write in one command buffer stays invisible to a read in the next.
  Both are unconditional per encoder.
  Emitting the consumer only where an *intra-list* barrier was needed is the subtler mistake: a list whose first op has no local hazard then never waits for the list that wrote what it reads.
  That passes in isolation, because the queue usually drains between two submits, and fails under load.
- **A binding group is one argument buffer, and the layout is `binding.index` directly.**
  Metal has no descriptor-set layout object and no root signature, so both layout types here are schema and make no device call at all.
  A group becomes one argument buffer whose 8-byte slot `n` is what `[[id(n)]]` addresses in MSL.
  A buffer holds a GPU address, a texture or sampler an `MTLResourceID`, and an array binding takes `count` consecutive slots.
  That is what SPIRV-Cross emits for a descriptor set, which is the whole reason the layout was chosen.
  A group holds every resource it names: an argument buffer is raw addresses, so nothing else keeps the target alive.
  A staging group is the CPU-side image of one such buffer, and a snapshot is a fresh buffer copied from it — which is what makes snapshots independent of the builder and of each other.
  It needs **one** descriptor array where dx12 needs two: an argument buffer has no view/sampler heap split, so both of the base's offsets are `binding.index`.
  They cannot collide, because sg already requires that index to be unique within its group.
- **There is no per-pipeline cached blob.**
  `cached_pipeline_data()` returns empty and `used_cached_pipeline()` is always false, so every pipeline build is cold.
  Metal 4's `MTL4Archive` is one store per *compiler* where sg's surface is one blob per *pipeline*, so the mapping
  vulkan found — a `VkPipelineCache` per pipeline — has no counterpart here.
  Recorded as open in [docs/TODO.md](../../docs/TODO.md), and pinned by a test so it reads as deliberate.
- **A raster pipeline is three objects where dx12 and vulkan have one.**
  MTL4 splits what a D3D12 PSO folds together.
  The render pipeline state carries the shaders, the vertex layout and the colour attachments' blending.
  The depth and stencil test is a separate `MTLDepthStencilState` bound on the encoder.
  Cull mode, fill mode, winding and depth bias are encoder calls rather than pipeline state at all.
  The third group is replayed on every `bind_pipeline`, which is what keeps it consistent with the pipeline a caller believes is bound.
- **Attachment formats are not pipeline state.**
  MTL4's render pipeline descriptor has no depth or stencil attachment format at all, and its colour formats are only what the blend descriptor needs.
  The render pass establishes the rest at encode time.
  sg's `depth_stencil_format` is therefore carried for validation rather than for building.
  That is the opposite of dx12's DSVFormat and vulkan's dynamic-rendering formats.
- **A barrier is flushed before the render encoder opens, not inside it.**
  Vulkan forbids a barrier inside a dynamic-rendering instance and closes the pass around one.
  Here the declares are simply flushed first, which costs nothing a frame that transitions its targets up front was not already paying.
- **A pipeline is built through an explicit compiler object.**
  MTL4 makes compilation an `MTL4Compiler` the context owns, where Metal 3 hid it behind the device.
  A metallib blob reaches it as `dispatch_data`, and the entry point is named through an `MTL4LibraryFunctionDescriptor` rather than looked up on the library.
- **Bindings reach a dispatch through an argument table, not per-encoder setters.**
  One `MTL4ArgumentTable` serves a command list, and a group bound at slot N writes its argument buffer's address into buffer-binding N — so sg's `group_index` *is* the MSL `[[buffer(N)]]` index.
- **A texture has no layout, so one access tracker serves both resource kinds.**
  dx12 and vulkan each need two — a texture's tracker carries its layout and partitions it by subresource — and here a
  texture has no state a buffer does not also have.
  `metal_resource_access` is that one type, and `current_texture_layout` answers `general` for every texture and range
  because that is true rather than a placeholder.
  `cmd.ensure_layout` is honoured as a no-op rather than asserting, so portable code may call it unconditionally, which
  is what it is for.
  The subresource partition is an optimization not taken: a texture is one undivided state, so two mips written and
  read in turn get a barrier they would not strictly need.
- **A texture view is cached on sg view identity, never on the texture's address.**
  That distinction is the vulkan build-out's most expensive bug repeated cheaply: a per-frame texture's address is
  recycled, so an address-keyed cache hands a new texture the previous one's view, of an object that no longer exists.
  It needs an allocator to reuse an address, so a suite reports it as flaky and a frame loop reports it every few
  seconds.
- **A barrier names stages, not resources.**
  `sg::pipeline_stage_flags` maps onto `MTLStages` directly: `vertex` to `MTLStageVertex`, `compute` to `MTLStageDispatch`, `copy` to `MTLStageBlit`.
  The resource list an sg barrier carries has nowhere to go.
- **Residency is declared, not inferred.**
  MTL4 has no `useResource`; a command buffer names an `MTLResidencySet` instead, which is what a list's touched-resource set becomes.
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
- **A Metal callback is a real thread even in a build with no threads.**
  `SC_THREADS=OFF` compiles `cc::mutex`'s lock away, which is correct for state only sg's own code touches.
  Every commit's `MTL4CommitFeedback` handler runs on a dispatch queue Apple owns, and that flag does not reach it.
  The residency set catches it immediately and fatally: `residency sets do not support concurrent write operations`, aborting the singlethreaded suite on the first async upload.
  `callback_mutex` in `metal_common.hh` is `cc::mutex`'s shape with a lock that is always real.
  Three pieces of state hold one — the residency set, the transfer system's pending map, the feedback sink's context pointer — and everything else keeps `cc::mutex`.
  `cc::atomic` has the same shape and the same hole, being a plain value with threads off, so the two counters a commit handler decrements are `std::atomic` and say why.
- **An async texture transfer needs no layout settling, where dx12 needs a whole command list for it.**
  A D3D12 copy queue cannot run layout barriers, so dx12 submits a direct-queue fixup before it stamps the job.
  Metal textures have no layout at all, so the off-frame path is the buffer path with a footprint: same queue, same two waits, `staging_layout_of` in place of a byte count.
- **Concurrent pipeline compilation aborts inside the driver.**
  Two threads in `MTL4Compiler::newRenderPipelineState` — on *different* compilers, from different contexts — abort in `_os_unfair_lock_corruption_abort` under `AGXG16GFamilyCompiler`.
  About one run in five on an M4 under macOS 26, and it is the driver rather than the validation layer: it reproduces with `MTL_DEBUG_LAYER=0`.
  So every pipeline build is taken under `pipeline_compilation_lock()`, process-wide rather than per context.
  The state being corrupted is the device's, and a Mac hands the same device to everyone who asks.
  `sg metal - pipelines build concurrently from several contexts` is the gate, and it is probabilistic: without the lock it takes the binary down within a run or two.
- **There is no software device.**
  dx12 has WARP and metal has nothing, so coverage here is developer-machine-only and a host below the floor makes every test `SKIP`.

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

### Commit feedback is the one programmatic channel

`MTL4CommitFeedback` reports a failure that arrives after the call that caused it, which is exactly what sg's deferred error channel is for.
So every commit carries a handler, and an error reaches `ctx.take_pending_errors()`.

Metal's timeout, device-removed and access-revoked codes mean the device itself is gone and mark the context lost; everything else is one command buffer failing.

The handler runs on a dispatch queue at a time nothing here controls, which can be after `shutdown` has returned.
So it captures a [`metal_feedback_sink`](src/shaped-graphics/backends/metal/metal_feedback.hh) rather than the context, and shutdown detaches it.
A handler still in flight then does nothing instead of reporting into freed memory.

## Testing

`shaped-graphics-metal-test` is the tier-2 binary: bring-up, the floor refusal, the epoch timelines, command-list lifetime, transfer, the bind path, compute, and what Metal reports where.

The tier-1 suite has **no compute execution test at all** — it cannot, because bytecode is per-backend by construction — so this tier is the specification for the dispatch path.
`double_compute.metal` is checked in beside the `double_compute.metallib.h` compiled from it, with the command line in the source's own comment.
So the fixture is reproducible by hand, and the binary needs neither the shader library nor the Metal toolchain.
Its reflection is written out by hand next to it, so the test states the binding shape it means rather than inheriting whatever a reflector produced.
Every test builds its own context, because the context is its subject.
The whole binary runs with API validation armed, so a violation anywhere in it ends the run.

The tier-1 API suite (`shaped-graphics-test`) now compiles on macOS for the first time, since `_sg_test_drivers` is non-empty there.
Its driver is `nx::config::disabled` while the backend is built out — registering builds the per-invocable aliases, so one API test runs against metal by being named exactly:

```bash
uv run dev.py test "sg - advances an epoch"
```

Take the `disabled` off once no seam aborts, the way vulkan's came off.
