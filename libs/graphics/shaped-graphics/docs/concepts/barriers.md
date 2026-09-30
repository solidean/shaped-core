# Concept: barriers & access-state tracking

sg tracks how each resource is accessed and inserts the GPU barriers that order those accesses.
The goal is **correct, minimal** barriers with **no explicit barrier API** for the caller.
Access is inferred from the operation, and the concurrency model lets several command lists record at once.

## Access is inferred, not declared

There is no public `declare_access`. What a resource is used as follows from the operation:

- `cmd.upload` ⇒ `copy_write` on the destination; `cmd.download` ⇒ `copy_read` on the source.
- `cmd.copy` ⇒ `copy_read` on src plus `copy_write` on dst, and a self-copy is one combined access.
- A dispatch, a draw or a trace ⇒ what the bound pipeline's code does to each bound view, which is its **footprint** (below).

The mapping lives in [access_inference.hh](../../src/shaped-graphics/barrier/access_inference.hh), so every backend agrees on the semantics.

### Access follows the footprint

**A pipeline carries its footprint: what its code does to each binding, and in which stages.**
A group is declared once and bound to many pipelines, so the access its layout allows is the union over all of them.
One pipeline usually does less: a `mut buffer` an entry point only loads from is a read to that pipeline, and a binding it never names is untouched.
[footprint.hh](../../src/shaped-graphics/barrier/footprint.hh) holds it; a compiled shader carries one, and pipeline creation resolves every stage's against the layout.

At an op, each bound view is declared with the footprint's access and stages, and its layout still comes from its view class.

- **An untouched binding is not declared at all** — no hazard, and no layout transition.
  That is sound only while the footprint covers everything the shader statically uses, which holds because SGL computes it on the tree its emitter prints.
- **A read through a writable view is `storage_read`**, not `shader_read`: D3D12 wants `UNORDERED_ACCESS` before any access through a UAV, whatever the shader does.
  A storage read and write together are the one access the API infers at first use, so they take the first-write freebie.
- **Stages narrow too**: a slot only the pixel shader reads is declared at `fragment`, so a barrier before it does not stall vertex work.

Where a footprint comes from decides how far it narrows:

- `exact` — SGL's own analysis, after inlining.
- `reflected` — a compiler's reflection: a binding it omits is untouched, one it keeps is used as its declaration allows.
- `none` — nothing is known, and every view counts as its class says: a writable view is written.

A footprint naming a binding the layout does not hold is keyed differently from it, and is not used at all rather than read as "untouched".
The inline-constants block is the exception: it is set on the list rather than bound, so a slot naming it is simply skipped.

**A check a shader edit can flip logs and degrades; it never asserts.**
Hot reload replaces a shader under a running program, so a declaration that disagreed with the old code may disagree with the new one at any frame.
Such a mismatch logs an error once per pipeline, binding and kind of mismatch, and falls back to a barrier that covers both sides.
Only a mistake in the host's own code — a declaration naming no bound array, an element out of range — asserts.

Access is inferred everywhere except where sg cannot see into the code, and there the caller says what it will do.

**Arrays / bindless.**
Element usage of a resource *array* bound to a shader cannot be inferred: the shader may index only some elements, or use them differently.
So the caller declares it explicitly, split by resource family since buffers carry no layout.
`declare_array_buffer_access` takes `array_buffer_access` `{index, access}`; `declare_array_texture_access` takes `array_texture_access`, which also names the required `layout`.
A declaration applies to the next dispatch or draw only, resolved by binding name against the bound groups' array elements and tracked exactly like an inferred scalar access.
The compute, raytracing and raster scopes each carry the pair, and an element is tracked at the stages the pipeline's code touches its array in, as a scalar binding is.
The footprint still says whether the code touches the array at all:

- an array the code never indexes needs no declaration, and one declared anyway is dropped;
- one it indexes and nobody declared logs an error, and every element is covered at those stages — one global barrier for the buffers, and a transition for each texture whose layout is wrong;
- one declared unused (an empty span) that the code writes is covered the same way;
- declarations that write nothing, for an array the code writes, log an error, and every declared element is covered for the code's write too;
- a declared access the code cannot perform logs an error, and the barrier covers the declaration and the code together.

A declaration narrower than the code element by element is the caller's to make: the footprint knows only what the code does to the array as a whole.

Declaring a vacant (null-handle) or out-of-range element, or an array no bound group holds, asserts.
See [bindings — array bindings](bindings.md#array-bindings).

**Foreign code, through a native scope.**
A vendor SDK (DLSS Ray Reconstruction, FSR Ray Regeneration) records onto the native command list with native
resources, and assumes they are already in the state it needs.
`sg::backend::dx12::dx12_native_scope::open(cmd, textures, buffers)` is how that is done without sg losing track: each
entry names its `access_flags`, and the scope transitions the resource into it and records that as its state.
Closing the scope forgets the list's bind state, since foreign code may have set its own descriptor heaps, root
signature and pipeline.
sg never rebinds by itself, so the caller must bind a pipeline again before the next draw or dispatch — one that does
not asserts.
A texture's layout follows from the access through `native_layout_for`, rather than being named separately.

Under-declaring corrupts the tracker — sg then believes a state the GPU is not in — so the scope hands out a native
resource only for a handle it declared, and the debug layer catches the rest.
dx12 today; vulkan when a member needs it.
See [shaped-rendering's denoising doc](../../../shaped-rendering/docs/denoising.md), which is what asked for it.

## The vocabulary is backend-neutral

[resource_access.hh](../../src/shaped-graphics/barrier/resource_access.hh) defines three vocabularies.
`access_flag` says what an op does (`shader_read`, `copy_write`, …), and `pipeline_stage_flag` says where (`compute`, `copy`, …).
Both are `cc::flags` sets — `access_flags` and `pipeline_stage_flags` — so a declared access carries several of each at once.
`texture_layout` says how the texels are arranged, and buffers are always `general`.
A texture uses `shader_texture` / `shader_image` / `render_target` / `depth_readonly` / `depth_readwrite` / `copy_src` / `copy_dst` / `present`.
None of it is any one backend's spelling, and each value documents its D3D12 and Vulkan mapping.
`is_unordered_write` marks the writes that need a hazard barrier — shader, copy and accel writes.
Color and depth *targets* are ROP-ordered freebies.

## Draws inside one render pass on webgpu

WebGPU emits no barriers sg can see, and it orders every pass after the one before it.
What it does not order is two draws of one render pass: a pixel shader writing a buffer, and the next draw reading it, may race.
So the webgpu backend ends the pass before a draw that touches what an earlier draw of the open pass wrote, or writes what one read, and reopens it with its targets loaded.
A vertex or index fetch is a read like any other, so a draw fetching a buffer an earlier draw of the pass wrote splits too.
Which draws write is the bound pipeline's footprint; draws that only read split nothing.
`render_pass_splits` counts every end and reopen in the middle of a rendering scope, whatever forced it.
A backend whose native pass cannot hold an operation splits around it — a copy recorded inside the scope, a barrier found only at a draw, or this hazard — and vulkan splits for all three.
Every backend counts its splits in the same stat.
The first split for each cause on a context also warns, naming the cause; `context::render_pass_split_warnings` is the switch.

## Minimal barriers: the three-timeline state

[resource_access_state.hh](../../src/shaped-graphics/barrier/resource_access_state.hh) is the reusable state machine a backend feeds declared accesses into.
It keeps three timelines, so read-after-read is free and only the *delta* of new work is synced:

- `curr_*` — what the next op will do (accumulated by `declare`, consumed by `flush`);
- `inflight_*` — everything issued since the last write / command-list start;
- `barriered_read_*` — the reads already synced against the last write.

`flush` compares `curr` against the in-flight state, returns the `access_barrier` to emit, and rolls the state forward.
It returns nothing for a freebie — a first write, a read with no writer in flight, or a read already barriered.

The first-write freebie has one exclusion, and it is the subtle one.
It rests on the backend inferring the access itself — D3D12 promotes a buffer out of `COMMON` on its first op — and a backend can only infer **one**.
An op that reads *and* writes the same resource has two, so it is always spelled out, however empty the timelines are.
Only a same-resource copy does that today (`cmd.copy` with `src == dst`); skipping its barrier left D3D12 assuming `COPY_DEST` and rejecting the source read.

The machine is **opt-in**: a backend that emits explicit barriers uses it, and a driver-barrier backend (opengl/webgl) ignores it.
Emission is entirely the backend's own; there is no core "emit this barrier" seam.

## A texture starts in a real layout, and gets there once

A texture is created in a layout no barrier may target — `VK_IMAGE_LAYOUT_UNDEFINED` on Vulkan — so a tracker that
started textures there would have nothing for a barrier to name as its source.

So a texture starts somewhere real.
`texture_description::initial_layout` names it, and `nullopt` derives one from `usage` — most-specific first, with
`general` last rather than default, since it is the one layout drivers cannot compress.
`undefined` and `present` are rejected: the first is the state this exists to leave, the second belongs to a swapchain
image.

The **one-time** transition out of `UNDEFINED` belongs to no list.
A list gathers, while recording, the textures it is the first to touch — a *tentative* set, since a concurrently
recording list may claim one first, so it is a superset and never a subset.
At **submit**, inside the submission lock, each is claimed against the texture and the survivors' `UNDEFINED -> resting`
barriers go into the small command buffer prepended to the same `vkQueueSubmit`, ahead of that list's entry barriers.
Whole-image rather than per-subresource, because a list that uses one mip leaves the others where `vkCreateImage` left
them while the tracker would say otherwise.

Claiming at submit rather than at record is the load-bearing part: a list that recorded second can submit first.

**An async upload can claim it too**, and on a fresh texture it usually does: whoever enqueues first wins.
It claims under the submission lock a list submit takes, and before returning it submits the one barrier on the
transfer queue — straight to the async-ready layout rather than the resting one — with a value of its own on the
texture's upload timeline.
The claim records that layout for the whole image, and it is true of the image in host submit order from then on,
because every submit that could trust it comes after that one.
Every list touching the texture waits on that value, which covers a list that lost the claim and one recorded after
it; a list after it transitions out from there through its ordinary entry barriers.
So creating a texture and streaming into it costs one small transfer-queue submit on Vulkan, and no direct-queue one.

**While the claim is owed, the tracked layout reads as `undefined`**, whatever the texture was seeded to start in.
What reads it is the async fixup below, and telling it the truth is what makes a download of a fresh texture — which
claims nothing — submit the list that claims the transition instead of copying from an image nothing has transitioned
yet.

dx12 needs none of this and ignores the field for the transition's purposes.
A D3D12 resource is created in `COMMON`, which *is* `general`, so its tracker's default is already true of the resource;
dx12 also expresses discard as `D3D12_TEXTURE_BARRIER_FLAG_DISCARD` on the barrier rather than as a layout.

## What orders what: the calls on the context

Everything below rests on one model, so it is worth stating before any of it.

**The events are the calls on the `context`**, and there are exactly two kinds:

- `ctx.submit_command_list` — a command list's recorded work enters the timeline;
- an async or streaming transfer's entry point — `ctx.upload.*`, `ctx.download.*`, `ctx.stream.*` — a transfer enters the timeline.

**Their call order is the order.**
A transfer enqueued before a list is submitted happens-before that list, and a list submitted before a transfer is enqueued happens-before that transfer.
Every wait, stamp and entry barrier exists to realize that order on the device; none of them defines it.

**Recording is not an event.**
A command list is recorded at one time and submitted at another, and only the submit is on the timeline.
So nothing observed while recording — what a resource's layout was, whether a transfer was in flight — is a fact the model gives meaning to.
A resource's state is resolved at submit for exactly this reason, and that is what the concurrency section below is about.

**The transfer systems' threading is below the line.**
A `cc::threaded_actor`, its windows, and when it happens to pick a job up are implementation.
They may not be reasoned about from outside, and a caller never has to.

This is the contract a backend implements rather than a description of what one does.
Where an implementation reads something the model does not define — a check taken during recording, say — that is a defect in the implementation whether or not it currently misbehaves.

## The transfer queue never moves a layout — the direct queue settles it first

An async or streaming transfer runs on a queue that cannot settle a texture's layout for itself.
A D3D12 copy queue **cannot run layout barriers at all**, so a copy there requires the resource in `COMMON`.
Vulkan's transfer queue can run them, and moving a layout there is still wrong for a different reason: the validation
layer tracks image layouts in `vkQueueSubmit` **call** order, so a transfer submit landing after a direct submit that
moved the image reads as a mismatch even when the GPU ordering is right.
Correct and unverifiable is still unshippable, since a layer message fails a test.

**The exception is the one-time transition out of `UNDEFINED`**, above, and what makes it one is *when* it is
submitted rather than which queue runs it.
It is submitted by the upload's own enqueue, under the submission lock, before anything that trusts it can be.
Recorded in the upload's first window instead, it reached `vkQueueSubmit` whenever the actor got to it, and every
submit in between — a list's, or the download actor's — named a layout the layer had not yet seen the image leave.

So for a texture a list has already used, the **direct queue** settles the layout before the transfer is enqueued.
`ctx.prepare_texture_for_async` compares the texture's current layout against what the transfer needs, and on a
mismatch submits a throwaway command list holding one transition.
The transfer then emits no layout-moving barrier, and has no layout claim for the layer to disagree with.

**It warns, once per texture.**
It fires where a command list moved the texture out of the async-ready layout, and where a download is the first thing
to touch a fresh Vulkan texture; a fresh texture's upload takes its own transition, and dx12 creates one in `COMMON`.
The caller could have avoided the submit by recording `cmd.prepare_for_async` on a list they already submit, and the
message says so.
There is no opt-out on purpose: doing that is both the fix and the thing the warning asks for.

The fixup runs **before** a transfer job's stamps rather than after.
It is an ordinary command list, so its submit waits on the texture's pending-transfer values — and a value stamped for
a job still being enqueued is one nothing will ever signal.

**The async-ready layout is `general` on both backends**, and `sg::async_direction` is accepted and ignored.
A direction-specific layout on vulkan would keep more compression and cannot be held: see [TODO](../TODO.md) for the
submit-call-order reason and what would earn it back.


## Subresources: a covering partition (designed-in for textures)

A texture's subresource domain is the grid of mip × array slice × aspect plane.
Buffers are single-subresource and never touch this.
[subresource.hh](../../src/shaped-graphics/resource/subresource.hh) is the range vocabulary.
The state kept over those ranges lives in [subresource_state.hh](../../src/shaped-graphics/barrier/subresource_state.hh).
That is a **covering partition** — a set of range-boxes that always exactly tile the whole domain.
Declaring an access to a sub-range *splits* boxes so the range aligns to box boundaries, keeping the tiling exact, then touches only the covered boxes.
`try_merge` collapses back to one box once every box's state is equal, so a texture used uniformly costs one entry rather than one per subresource.
It is an explicit call after each flush, not something the partition does on its own.
A backend keeps one partition per open command-list slot, plus one for the current between-lists state.

## Concurrent command lists (the concurrency model)

Every command list is "concurrent": on creation it takes a **slot** from the context's [command_list_slot_allocator](../../src/shaped-graphics/barrier/command_list_slot.hh).
The allocator is a mutex-guarded 64-bit free bitmask — lowest clear bit — with a heap free-list past 64 that warns, since that many concurrent recorders usually means a leaked list.
The slot keys the list's **private** access-state entry inside each resource it touches: a `cc::small_vector` of per-slot states, so a few parallel lists do not allocate.

**A slot starts empty.**
A resource enters at whatever the list's own first op asks for — its layout, its stages, its access — recorded as that
list's **entry requirement** and nothing else.
Nothing the list records is trusted about where the resource starts, which is the property the whole model is after.

Each resource also carries a **current** state: what it is in as of the last *submitted* list.
At **submit**, inside the submission lock, per touched resource:

- the entry requirement is resolved against the current state, and the barrier that satisfies it goes into a small
  command buffer **prepended** to the same submit — `_pre_buffer` on vulkan, a second command list on dx12;
- the list's final state then becomes the current one, unconditionally, in submission order.

Only the boxes a list actually used are committed: a slot's partition starts empty, so its untouched boxes say nothing
about the subresources the list never named.

Under the lock, because submission order is what that lock serializes — it is what makes "the state so far" mean
anything at all.

**What this replaced, and why.**
The model before it seeded a slot from the shared state at its first touch, and had every non-last finalize *revert*
the resource to it.
Two things were wrong with that.
A list computed its barriers against whatever the resource was in *while it recorded*, so two lists recording
concurrently never saw each other's declares: A's write and B's read both took the no-barrier freebie, and submitting
A then B left them unsynchronized.
Vulkan gives no implicit ordering between two `vkQueueSubmit` batches, so that was a real hazard, and synchronization
validation reports it as a `READ_AFTER_WRITE`.
(dx12 was safe there by construction: `ExecuteCommandLists` orders the batches, and a buffer decays to `COMMON`.)
And revert held only as long as command lists were the only things moving a layout — an async transfer's fixup moving
one between a list's submit and another's left the second list's barriers naming a layout the texture had left.

**Buffers keep their between-lists state on vulkan and none on dx12.**
D3D12 decays a buffer to `COMMON` at `ExecuteCommandLists`, so cross-list ordering is free there and only *intra-list*
hazards ever need a barrier.
Vulkan has no such decay, so a vulkan buffer carries the same current state and entry requirement a texture does, minus
the subresource partition.

## dx12: buffer barriers + texture layout transitions

dx12 tracks intra-list hazards with the shared state machine and emits **enhanced barriers** (`ID3D12GraphicsCommandList7::Barrier`).
See [dx12_barrier.hh](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_barrier.hh).
Uploading then downloading — or self-copying — the **same** buffer works in one command list, with a precise `COPY_DEST→COPY_SOURCE`-style transition rather than a bounce through `COMMON`.

**Access is declared, then flushed, then emitted — batched per operation.**
An operation first *declares* access on every resource it touches — a copy's src + dst, or a dispatch's whole bound group.
That only accumulates into each resource's next-op state, emitting no barrier.
`flush_barriers()`, just before the op, then flushes each declared resource, turning its accumulated declares into barriers.
It submits the whole batch in one `Barrier` call, with one `D3D12_BARRIER_GROUP` per type.
A dispatch binding many resources pays one barrier call, not one per binding.
And a resource bound *more than once* to the same op — two views of one texture, say — merges its declares into a single barrier carrying the **union** of the accesses.

When those bindings need *different* layouts — a texture bound as both a texture (`shader_texture`/SRV) and an image (`shader_image`/UAV) view — `combine_layouts` picks one that serves both.
No specialized D3D12 layout serves both an SRV and a UAV, so it falls back to `general` (COMMON) and warns once, since sampling in COMMON is slower.
A genuinely incompatible pair, such as copy-dest plus sampled in one op, asserts.

For **buffers specifically the concurrency machinery is teeth-free**: a dx12 buffer's layout is always `general`, because D3D12 decays buffers to `COMMON` at `ExecuteCommandLists`.
So a buffer keeps no between-lists state at all, and cross-list ordering rides on that decay.

**Textures give the machinery teeth.**
Each `dx12_texture` owns a per-command-list covering partition, [dx12_texture_access](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_texture_access.hh).
`declare` accumulates the covered subresource boxes' access.
`flush` rolls them through the state machine and returns the per-box `D3D12_TEXTURE_BARRIER`s the command list batches and emits before the op.
Each is scoped to a `D3D12_BARRIER_SUBRESOURCE_RANGE`, carrying `LayoutBefore→LayoutAfter`.
A submit returns the box's **entry** transitions, which go into a second command list executed ahead of this one in the same `ExecuteCommandLists` call.
That list is created only when there is a transition to put in it, since a command list and its allocator are not free and most submits need none.
Never into the list's own body, since a list that recorded second may submit first.
This is dx12-owned end to end — SG core hands out no barriers, only the neutral state machine and partition.
Barrier models differ enough across backends (Vulkan image layouts / aspects / queue ownership) that each owns its tracking and emission.
The drivers today are the inline texture copy, the compute, ray-tracing and raster bound groups, the raster rendering scope's target transitions, and the swapchain's present transition.
The inline copy is the clearest: `cmd.upload.bytes_to_texture` / `cmd.download.bytes_from_texture` record a `copy_dst` / `copy_src` access against the region before staging.
The layout transition is therefore emitted ahead of the `CopyTextureRegion`.
The **vulkan** backend reuses the shared vocabulary and state machine with its own emission, and its own prepended command buffer.

## See also

- [resource_access.hh](../../src/shaped-graphics/barrier/resource_access.hh) — the neutral vocabulary.
- [resource_access_state.hh](../../src/shaped-graphics/barrier/resource_access_state.hh) — the three-timeline machine.
- [subresource_state.hh](../../src/shaped-graphics/barrier/subresource_state.hh) — the covering partition.
- [command_list_slot.hh](../../src/shaped-graphics/barrier/command_list_slot.hh) — the concurrency substrate.
- [tests/vulkan-concurrent-lists-test.cc](../../backends/vulkan/tests/vulkan-concurrent-lists-test.cc) — two lists recorded against one resource, run under synchronization validation.
- [threading](threading.md) — the thread model concurrent recording builds on.
