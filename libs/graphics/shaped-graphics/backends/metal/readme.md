# The metal backend

`sg::backend::metal` — shaped-graphics on Metal 4, for macOS and iOS.

Early stage.
The device, the queue, the epoch timelines, the command-list lifecycle, buffers and memory heaps are real.
Recording, textures, bindings and both transfer paths still assert.
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
- **Placement works for textures from the start.**
  A Metal placement heap is not told what it will hold, so there is no buffers-only stage to grow out of the way dx12 has one.
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

`shaped-graphics-metal-test` is the tier-2 binary: bring-up, the floor refusal, the epoch timelines, command-list lifetime, and what Metal reports where.
Every test builds its own context, because the context is its subject.
The whole binary runs with API validation armed, so a violation anywhere in it ends the run.

The tier-1 API suite (`shaped-graphics-test`) now compiles on macOS for the first time, since `_sg_test_drivers` is non-empty there.
Its driver is `nx::config::disabled` while the backend is built out — registering builds the per-invocable aliases, so one API test runs against metal by being named exactly:

```bash
uv run dev.py test "sg - advances an epoch"
```

Take the `disabled` off once no seam aborts, the way vulkan's came off.
