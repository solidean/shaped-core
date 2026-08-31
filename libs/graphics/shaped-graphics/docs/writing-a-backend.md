# Writing a backend

The order to build an sg backend in, and what the second implementation learned that the first could not tell you.

[concepts/backends.md](concepts/backends.md) says what a backend *is*.
[coding-guidelines](coding-guidelines.md) carries the rules it must follow.
[testing](testing.md) says which tier a test belongs in.
This is the procedure on top of them: what to do first, what order the pieces unlock each other in, and which parts cost more than they look like they will.

**It is written from the vulkan build-out and grows with it.**
dx12 came first and could not tell you which of its choices were D3D12 leaking into the abstraction and which were sg being right — only a second backend can, and only while it is being written.
A section appears here in the same commit as the milestone it describes.

---

## Do these three things before any rendering code

Each pays for itself inside a day, and each is easy to postpone into never.

### 1. Register the test driver, even though nothing works yet

The cross-backend tier-1 suite is the single best oracle you have.
On a platform without a mature backend it is **not compiled at all**: `shaped-graphics/CMakeLists.txt` gates all 19 topic files on `_sg_test_drivers` being non-empty.
Until your backend is in that list, there is nothing to run.

Register the driver and keep `nx::config::disabled` on it.
The two flags do different jobs, and the distinction is the whole trick:

- **registering** builds the per-invocable aliases, so any one API test runs against your backend by being named exactly;
- **disabled** keeps a full sweep out of the seams you have not reached.

Nexus's orphan check exempts an alias-reachable invocable for exactly this case, so the suite stays green while you grow.
See [`vulkan-entry.cc`](../tests/backends/vulkan-entry.cc) and [nexus/docs/invocable-tests.md](../../../base/nexus/docs/invocable-tests.md#orphan-safety-net).

**Expect this to find bugs immediately.**
Vulkan's first probe passed 52 of 135 invocables.
The first tier-1 test run against it caught a real defect: `advance_epoch` never called `apply_pending_transient_budget()`, so `ctx.transient.set_budget()` was silently inert.
The test for it had existed the whole time.

### 2. Wire the validation layer into the test framework

Not at the end, as a parity item — first, as the primary oracle.
[testing](testing.md) records what the alternative looks like: dx12 accumulated roughly 680 unnoticed validation messages before it grew a listener, with the suite green throughout.

A backend under construction is wrong in exactly the ways a validation layer checks: synchronization, image layouts, descriptor state.
So this converts most mistakes from "the image is black" into "this named test failed, here is the rule you broke".

**Then prove the listener fires.**
A listener nobody has seen fire is indistinguishable from one that is not connected, and "no validation errors" is a claim about the backend only once you know the wiring works.
Provoke a real violation in a test and check the callback saw it — a zero-size `vkCreateBuffer` is a pure diagnostic with nothing to clean up.

### 3. Turn "no device" into SKIP, not into a passing test

A suite that silently passes when it found no device gets more dangerous the more it covers.
Only dx12 has a guaranteed software adapter; plan for your backend's coverage being developer-machine-only and say so.

---

## The milestone order

Near-forced by the dependencies.
Each line is what the next one needs.

1. **Device + infrastructure.** Feature enablement, the validation listener, test-driver registration, memory-type selection.
2. **Barriers + inline transfer.** The first milestone that turns a large block of tier 1 green, and the first that needs no shader.
3. **Binding path + compute.** The first that needs a working shader toolchain for your bytecode format.
4. **Raster.**
5. **Ray tracing.**
6. **Presentation.**
7. **Async transfer + streaming.** The largest single piece, and the only one needing a second queue.

### What actually depends on what

The list above is the order the work is *described* in.
The order it was *built* in differed, and the differences are the useful part — each is a dependency that looked real in a plan and was not, or one that did not appear in a plan at all.

- **The shader toolchain is not a prerequisite for the bind path.**
  This is the biggest one.
  Only compute *pipelines* need compiled shaders; group layouts, the descriptor storage, binding groups and staging groups need none.
  The vulkan build-out planned the toolchain before the bind path, then built layouts, the descriptor-buffer foundation, the heap and the view translation *first*.
  The toolchain landed in the middle of that, and nothing had to be reordered.
  A backend whose bytecode format already has a compiler can skip the question entirely; one that does not should not let it block half a milestone.

- **The memory heap is independent of everything and unblocks a whole topic.**
  `ctx.transient` bump-allocates from one, so until it exists every transient test fails for a reason that has nothing to do with transient resources.
  It has no dependency on barriers, transfers or bindings, so it can land as early as the device does.

- **Registering the test driver comes before all of it**, because nothing else is measurable until it does.

- **Host-visible memory is the real gate on transfers**, not barriers.
  Barrier translation and access tracking are device-free and can be written and tested before any transfer plumbing exists; what actually blocks a copy is having somewhere for the CPU to write.

- **Within transfers the order is: barriers, then buffers, then textures.**
  Buffer upload and download share a ring and a tracker with the copy path, and texture transfer needs layout tracking on top of all of it.
  Doing textures first means building the layout tracker with nothing able to exercise it.

- **Device features are the one thing genuinely worth doing up front**, since everything above depends on some of them and nothing depends on the order they were enabled in.

So the real shape is less a ladder than three chains that only meet near the end:

```text
device + features + test driver
   ├── barriers → buffer access → inline transfer → texture access → texture transfer
   ├── memory heap → transient
   └── layouts → descriptor storage → binding groups ─┐
                                                       ├── compute pipelines → dispatch
       shader toolchain ───────────────────────────────┘
```

**Enable every device feature up front**, in one commit, whether or not the milestone using it has landed.
A feature costs nothing unused, and adding them one at a time means re-editing the same struct chain five times.

**Put the version/capability floor in as a hard requirement rather than a per-capability probe.**
sg already makes this call for storage-buffer offsets in [concepts/views.md](concepts/views.md).
The alignment is a portable rule hardcoded rather than queried, so it "fails loudly on a dx12 dev box rather than surfacing later".
The same reasoning applies to a backend's own floor: a capability that is probed and branched on is a second code path that gets exercised on nobody's machine.
Refuse below the floor with one error that **names what is missing** — "this GPU is too old" is not something a caller can act on.

---

## Which sg abstractions turned out portable

The interesting half.
Recorded as each is met, because this is what the next backend most wants to know.

- **The access/barrier vocabulary is genuinely backend-neutral.**
  `barrier/resource_access.hh` annotates every `access_flag` and `texture_layout` with its D3D12 *and* Vulkan spelling.
  `resource_access_state::flush()` returns an `access_barrier` whose fields map one-to-one onto `VkMemoryBarrier2`.
  The translator is a mapping function, not a design problem.
- **`sg::binding` already separates the two ways a language namespaces a binding.**
  `group_index` is a hardware-visible descriptor set; `space` is a register-numbering namespace that never reaches the descriptor table.
  A second backend fills the other field and nothing else changes.
- **`raster_begin_rendering` is already shaped as dynamic rendering**, with attachments named at record time and no render-pass object in the API.
- **The epoch system carries across cleanly.**
  Timeline semaphores are a closer fit than the fence-plus-event model dx12 needs.
  `command_list_slot` was already the right seam for concurrent recording.

### And where it forks

- **Cross-list buffer state.** dx12 keeps none: D3D12 decays a buffer to `COMMON` at `ExecuteCommandLists`, so cross-list ordering rides on that decay and `dx12_buffer::finalize_slot` is a no-op.
  Vulkan has no decay, so the last writer must survive its own command list.
  The fix was not a new design — it is the canonical/promote model dx12 already uses for *textures*, minus the subresource partition.
  **If your API lacks an implicit-decay rule, expect this.**
- **The layout a resource is *created* in is not the same question on every API.**
  dx12 seeds its texture tracker's canonical state with `general`, because a D3D12 resource is created in COMMON and that is what `general` means.
  A Vulkan image can only be created `UNDEFINED` or `PREINITIALIZED`, so the same seed would have the first barrier declare an old layout the image is not in — which Vulkan rejects.
  Check what your API's creation call actually leaves the resource in, and seed the tracker with that rather than inheriting the reference backend's answer.
- **A register-based API namespaces a binding address twice; a descriptor set does not.**
  HLSL numbers a register in a *space* and in a *class* (`t`/`s`/`u`/`b`), so a reflected binding list routinely holds
  several bindings at index 0 and D3D12 resolves them at layout build.
  SPIR-V, WGSL and Metal have one namespace per group, so the same list is not a set layout at all.
  sg's rule — `index` is the address within its group, and two bindings must not share one — was implicit until a
  second backend needed it; see [concepts/bindings.md](concepts/bindings.md).
  **Expect the reference backend's own tests to encode its namespacing**, and read a shared test's layout before
  assuming your backend is what is wrong.

- **A barrier may be illegal where the reference backend flushes one.**
  dx12 flushes a draw's barriers inside the render pass, right before the draw, exactly as it does for a dispatch.
  Vulkan forbids `vkCmdPipelineBarrier2` inside a dynamic-rendering instance outright (VUID-vkCmdPipelineBarrier2-None-09553), so the same code is a validation error.
  The answer that keeps sg's API intact is to close the instance around the barrier and reopen it with every load op
  forced to LOAD — cheap when it never happens, and a tile flush when it does.
  Bound pipelines, descriptors, vertex buffers and dynamic state are command-buffer scoped rather than instance
  scoped, so none of it has to be replayed.
  **A frame that transitions its resources before the scope opens never pays this**, which is worth saying in the
  backend's own docs rather than leaving as a surprise.

- **An acceleration structure is an object here and an address there.**
  DXR names a structure by the GPU address of its storage buffer, so dx12's `blas`/`tlas` subclasses hold nothing but
  a typed handle to that buffer.
  Vulkan needs a `VkAccelerationStructureKHR` created over the buffer, with a device address of its own — so the
  subclass owns an object, and the ownership question ("what frees this, and when") appears where dx12 has none.
  The sg-level policy is untouched: result persistent, scratch transient, and the AS access bits illegal on non-AS
  buffers.

- **`used_cached_pipeline()` looked like an sg-surface gap and was not.**
  dx12 answers it precisely because D3D12 fails PSO creation on a blob it cannot use, while Vulkan silently starts
  from an empty cache — so "did creation succeed" carries no information there.
  The exact answer comes from somewhere else: a pipeline-cache blob's header is a *specified* structure, carrying the
  version, vendor, device and cache UUID a driver checks, so the backend runs the same check before handing the blob
  over and reports what it found.
  **Look for a spelled-out rule the API expects you to apply yourself** before concluding a question is unanswerable —
  an escalation that turns out to be a missing lookup costs the whole surface a needless change.

---

## Costs that surprised us

- **The root blocker for every transfer path is one memory-type decision.**
  All twelve of vulkan's transfer stubs traced back to the backend never requesting a host-visible memory type: there was nowhere for the CPU to write bytes a GPU copy could read.
  Find this early; it looks like twelve problems and is one.
- **Creation-path ownership transfer is a double-free waiting to happen.**
  A scope guard that unwinds partial creation must be disarmed the moment the context object is constructed, not at the end of the function.
  From construction onward the context's destructor owns those handles, and a later failure would otherwise free them twice.
- **Not every sg scope validates before it reaches you.**
  `cmd.upload.bytes_to_buffer` forwards straight to the backend seam with no checking at all, so the backend owns the null, bounds, usage and expiry contract.
  The trap is that a `CC_UNREACHABLE` stub *satisfies* the `CHECK_ASSERTS` tests for those contracts, so they pass while unimplemented and regress the moment you implement the seam.
  Copy the reference backend's assert list rather than inferring it, and mind the ordering: bounds are checked before the empty-input early-out, so an empty write at a bad offset is still a violation.
- **A reference-counted device hides teardown-order bugs.**
  dx12's memory heap holds a `ComPtr<ID3D12Device>`, so the device simply outlives it and nothing has to order the two.
  A `VkDevice` is not reference counted, so anything holding device memory has to be released *before* it.
  sg's transient bump heap was not, and the validation layer reported it as a leaked `VkDeviceMemory` at `vkDestroyDevice`.
  The fix belonged in sg rather than the backend: `context::shutdown` already clears routines for exactly this reason, and the transient heap now goes the same way.
  Wherever the reference backend gets a lifetime for free, check whether yours does.
- **A reference-counted device hides teardown-order bugs, and it hides more than one.**
  The first was the transient heap; the second was the pipeline cache, which holds binding-group layouts and pipelines
  that no caller still references.
  The pattern generalizes: **anything sg caches for the context's life is a teardown-order bug waiting on a backend
  whose device is not reference counted.**
  Both fixes belonged in sg rather than the backend — `context::shutdown` now releases the cache the way it already
  cleared routines — so look for the *category* on the first one rather than fixing them one validation message at a
  time.

- **Your own compiler's output decides what the device floor really is.**
  DXC emits the `RayQueryKHR` capability into every ray-tracing SPIR-V module it produces, used or not.
  So a device with ray-tracing pipelines but no ray query could not load a single shader our toolchain compiles, and
  ray query belongs in the required set rather than beside it as a separate probe.
  **Check what the compiler actually emits before deciding which capabilities are optional** — a feature nothing in
  the source asks for can still be a hard requirement.

- **A per-pipeline cached blob maps onto a per-pipeline VkPipelineCache.**
  Vulkan's cache is normally one shared object, and sg's surface is one blob per pipeline, so each pipeline owns a
  cache it was built with and serializes on request.
  It is not the idiomatic Vulkan shape, and it is the honest one for the contract sg states.

- **Objects a descriptor merely names want a per-context cache, not per-group ownership.**
  A dx12 sampler descriptor leaves no object behind, and D3D12 creates a view straight into a heap.
  Vulkan needs a VkSampler and a VkImageView that outlive every group holding them, and giving each group its own
  would mean deferring their destruction behind every group's epoch.
  Caching them per context makes the lifetime trivial and a re-minted group free.
  Key the cache on sg's own identity for the value — `sg::impl::sampler_hash`, `hash(raw_texture_view)` — rather than
  on one the backend invents, or the cache answers a different question than the layout identity does.

- **Keep translation logic device-free, and it becomes testable everywhere.**
  Barrier translation and access tracking are pure logic with no device in them, so their tests run on any machine rather than only where a device exists.
  On a platform with no software adapter that is the difference between covered and skipped, and it is worth splitting files along that line deliberately.

---

## Where the tier-2 suite carries the whole weight

[testing](testing.md) puts backend-specific behaviour in tier 2 and API invariants in tier 1.
One consequence is easy to miss until you go looking for a dispatch test: **the tier-1 suite has no compute, raster or
ray-tracing execution test at all**, because none of them can be written without shader bytecode, and bytecode is
per-backend by construction.

So the reference backend's tier-2 suite is the specification for those, and yours is written beside it:

- **Embed a compiled blob rather than building one.** dx12 checks in `double_compute.dxil.h` next to its `.hlsl`, with
  the compiler command line in the source's comment; vulkan does the same with `double_compute.spirv.h`.
  It keeps the tier-2 binary free of the shader library and the compiler toolchain, and it makes the fixture
  reproducible by hand.
- **Write the reflection by hand next to it**, so the test states the binding shape it means rather than inheriting
  whatever a reflector produced.
- **Size a resource-exhaustion test so that it actually exhausts**, then check that it does by breaking it on purpose.
  A free-list test on a region large enough to fit every allocation passes without touching the free list, and reads
  exactly like one that works.
- **Write the test for the path you invented, not only for the path that works.**
  The raster suspend/reopen above was written, and a first raster test passed without ever reaching it — every draw in
  it needed no barrier.
  The test that did reach it found a dangling `pName` in the pipeline's stage array on its first run, which the
  passing test had been getting away with.

- **Run every milestone against the single-threaded preset, not only the default one.**
  `SC_THREADS=OFF` is a whole-build switch that `check` exercises, and it changes who drives an async graph.
  Raster's first run there found a finished pipeline still referenced by the no-threads pool's queue, so a
  `VkPipelineLayout` outlived `vkDestroyDevice` — a clean-core fix, found only because the mode is gated.
  A backend leans on `ctx.cached.acquire_*` for every pipeline, which makes it the consumer most exposed to how that
  tier behaves in each mode.

- **A present handshake may need something from submit, or nothing at all.**
  DXGI gates back-buffer reuse with a fence signaled *after* Present, so dx12's swapchain needs no hook into command
  submission whatsoever.
  `vkAcquireNextImageKHR` signals a semaphore the first submit must wait on, and `vkQueuePresentKHR` waits on one that
  submit must signal — so the vulkan command list carries two semaphore fields, null on every ordinary list.
  sg's split of the handshake into `record_present_transition` + `present` around the submit is what leaves room for
  that, without either backend learning about the other's model.

- **Presenting with no window may be a real surface rather than an emulation.**
  `VK_EXT_headless_surface` gives a genuine `VkSwapchainKHR` with no display, so the headless path exercises every step
  the windowed one does.
  DXGI has no counterpart, so dx12 emulates with plain render-target textures and a rotating index.
  **Check for the real thing before writing the emulation** — the coverage is not close.

## Answer a capability query for what the backend can do, not what the hardware can

`cmd.raytracing.is_supported()` is the model for any "can this backend do X" seam.

The vulkan backend enabled the ray-tracing extensions at device creation — milestones ahead of using them — while
every build and dispatch seam was still a stub.
Reporting the *device's* answer then would have turned a clean skip into a crash, and told a caller nothing it could
act on.
So the context held the device's answer and the command list reported `false`, with a comment saying why, and a
tier-2 test pinned the gap as deliberate rather than an omission.

When the seams landed, the list started reporting the context and that test was rewritten to pin the agreement.
**Both halves matter**: a test that pins a temporary divergence is what stops it being read as a bug, and rewriting it
is part of finishing the milestone rather than a chore left behind.

The context's own answer is worth making stricter than the extension list: it is true only once the entry points have
resolved too, so a driver advertising an extension it does not implement reports false rather than crashing later.

## The pointer-into-a-growing-vector trap

Vulkan's create-info structs are a graph of pointers into caller memory, all of which must stay valid until the create
call returns.
That makes one C++ mistake unusually easy and unusually quiet:

```cpp
modules.push_back(...);
stages.push_back({.pName = modules.back().entry_point.c_str()}); // dangles once `modules` grows
```

The freed memory is usually still readable, so the wrong thing often *works* — the failure surfaced here as a
validation message about an entry point that was in fact present, and then a segfault, in the second test to use the
path.
**Collect every owner first, then build the structs that point into them**, and keep that shape even where the counts
look small enough not to matter.

## Write an example the moment the backend passes the suite

The vulkan backend passed all 135 tier-1 tests and the whole `check` gate, and then a first example — a rotating cube,
about 300 lines — found four bugs in an afternoon.
Every one of them is a lifetime or reuse question, and every one is invisible to a suite by construction.

- **A per-frame transient texture released before its list is submitted.**
  Vulkan's touched-resource lists held raw pointers where dx12 held handles, so the per-list access state was
  finalized on a freed object.
  A test builds a resource, uses it and drops it in one scope; a frame loop drops it *mid*-scope, every frame.
- **A view cache keyed on a resource address.**
  The address of a per-frame texture is recycled, so a new texture inherited the previous one's `VkImageView` — of an
  image that no longer existed.
  Intermittent by allocator luck, which is exactly the failure a suite reports as flaky and a frame loop reports every
  few seconds.
- **A query pool returned to its free list at submit rather than at epoch retire.**
  It is reset on the host when next leased, and that is illegal while a pending command still names it — which a
  single-submit-then-wait test never exercises.
- **An aspect index read as a `texture_aspect` value.**
  Plane 0 is `color` on a color format and `depth` on a depth one, so the first depth attachment anywhere produced a
  barrier with the wrong aspect bit.
  No tier-1 test had a depth target.

The common shape is that **a test runs each path once and a frame runs it sixty times a second**, against resources
whose addresses and slots are recycled.
So the example is not a victory lap — it is the first test of reuse, and it is cheap.
Write one as soon as raster works, run it under `--capture` (which needs no display) and under the sanitize preset,
and repeat it a dozen times: the intermittent ones only show up in a batch.

## House conventions that bite a newcomer

Small, and each costs a build cycle to rediscover.

- **Skim [clean-core's cheat sheet](../../../base/clean-core/cheat-sheet.md) before writing code**, not after.
  `cc::vector` has no `resize`: construction is `create_defaulted` / `create_filled` / `create_uninitialized`, and resizing is `resize_to_*` / `clear_resize_to_*` / `resize_down_to`.
  For the enumerate-then-fill pattern every graphics API uses, `create_uninitialized` is the right one — the driver overwrites every byte.
- **`CC_ASSERT` takes two arguments**, a condition and a message.
  The `cond && "message"` idiom fails to compile.
- **`cc::memcpy` lives in `clean-core/common/utility.hh`**, and is the blessed form.
- **clang-format rewrites `namespace sg::backend::x { struct y }` into the qualified `struct sg::backend::x::y`**, which then requires a forward declaration in the backend's `fwd.hh`.
  Declare every backend type there; it is the convention anyway.
- **A precompiled header hides a missing include.** A header that compiles inside its `.cc` can still fail standalone, and clang-tidy or a `nopch-*` preset is what catches it.
- **The prose linter enforces one semantic point per line** in comments as strictly as in docs, and a reflowed comment block trips it.
  See [prose](../../../../docs/guides/prose.md).
- **A test name containing a comma needs the comma escaped** on the command line (`dev.py test 'sg - a\, b'`), because
  a filter argument is comma-separated the way Catch2's is.
  It matters here because an exact name is the only thing that selects a test a disabled driver would otherwise skip.

---

## See also

- [concepts/backends.md](concepts/backends.md) — what a backend is, and why we duplicate rather than abstract.
- [testing](testing.md) — the two tiers, and which one a finding belongs in.
- [structure](structure.md) — the tagged roadmap; update your backend's tags per milestone, not at the end.
- [TODO](TODO.md) — where an unresolved divergence goes.
