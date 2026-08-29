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
- **`used_cached_pipeline()` cannot be answered exactly on every API.**
  dx12 reports it precisely because D3D12 never silently ignores a cached PSO, and `VkPipelineCache` does.
  This is an sg-surface question rather than a backend detail, so raise it rather than approximating.

---

## Costs that surprised us

- **The root blocker for every transfer path is one memory-type decision.**
  All twelve of vulkan's transfer stubs traced back to the backend never requesting a host-visible memory type: there was nowhere for the CPU to write bytes a GPU copy could read.
  Find this early; it looks like twelve problems and is one.
- **Creation-path ownership transfer is a double-free waiting to happen.**
  A scope guard that unwinds partial creation must be disarmed the moment the context object is constructed, not at the end of the function.
  From construction onward the context's destructor owns those handles, and a later failure would otherwise free them twice.
- **Keep translation logic device-free, and it becomes testable everywhere.**
  Barrier translation and access tracking are pure logic with no device in them, so their tests run on any machine rather than only where a device exists.
  On a platform with no software adapter that is the difference between covered and skipped, and it is worth splitting files along that line deliberately.

---

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

---

## See also

- [concepts/backends.md](concepts/backends.md) — what a backend is, and why we duplicate rather than abstract.
- [testing](testing.md) — the two tiers, and which one a finding belongs in.
- [structure](structure.md) — the tagged roadmap; update your backend's tags per milestone, not at the end.
- [TODO](TODO.md) — where an unresolved divergence goes.
