# The metal backend

`sg::backend::metal` — shaped-graphics on Metal 4, for macOS and iOS.

Early stage.
The device, the queue, the epoch timelines and the command-list lifecycle are real; every resource, binding and recording seam still asserts.
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
- **There is no software device.**
  dx12 has WARP and metal has nothing, so coverage here is developer-machine-only and a host below the floor makes every test `SKIP`.

## Validation has no callback here, and that is the open problem

This is the sharpest difference from dx12 and vulkan, and the reason `metal-entry.cc` registers a driver with no listener on it.

Both other backends install a callback that fails whichever test provoked a validation message.
[writing-a-backend](../../docs/writing-a-backend.md) puts wiring one up second on its list of three things to do before any rendering code.
The reason is that a backend under construction is wrong in exactly the ways a validation layer checks.

Metal offers no equivalent.
What it has instead:

- **`MTL_DEBUG_LAYER` and `MTL_SHADER_VALIDATION`** are environment variables the framework reads before any of our code runs.
  The checks they enable log to stderr and abort rather than calling back.
- **`MTLLogState`** does take a handler (`addLogHandler`).
  But it carries shader `os_log` output and the framework's own log channel — not, as far as we have established, the validation layer's messages.
- **`MTL4CommitFeedback`** reports a per-commit `NSError` after the fact, which catches GPU faults rather than API misuse.

**Establishing which of these actually delivers a validation message, and proving the listener fires on a deliberate violation, is the next piece of work here.**
A listener nobody has seen fire is indistinguishable from one that is not connected — which is why `metal_config` carries no validation knob and there is no `set_message_callback` yet.
Half-wiring one would look like an oracle and be none, and that is worse than the gap being visible.
Until it is settled the tier-2 suite's own assertions are the only oracle, which is weaker than what the other two backends had at the same stage.

## Testing

`shaped-graphics-metal-test` is the tier-2 binary: bring-up, the floor refusal, the epoch timelines and command-list lifetime.
Every test builds its own context, because the context is its subject.

The tier-1 API suite (`shaped-graphics-test`) now compiles on macOS for the first time, since `_sg_test_drivers` is non-empty there.
Its driver is `nx::config::disabled` while the backend is built out — registering builds the per-invocable aliases, so one API test runs against metal by being named exactly:

```bash
uv run dev.py test "sg - advances an epoch"
```

Take the `disabled` off once no seam aborts, the way vulkan's came off.
