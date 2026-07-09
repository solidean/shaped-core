# Concept: barriers & access-state tracking

> Concept docs answer **"what is this and why is it shaped this way?"** — the load-bearing design
> decisions, not the full API (that's the [cheat-sheet](../../cheat-sheet.md)). See also
> [threading](threading.md) and [epochs](epochs.md).

sg tracks how each resource is accessed and inserts the GPU barriers that order those accesses. The goal
is **correct, minimal** barriers with **no explicit barrier API** for the caller: access is inferred from
the operation, and the concurrency model lets several command lists record at once.

## Access is inferred, never declared (with one exception)

There is no public `declare_access`. What a resource is used as follows from the operation:

- `cmd.upload` ⇒ `copy_write` on the destination; `cmd.download` ⇒ `copy_read` on the source;
  `cmd.copy` ⇒ `copy_read` on src + `copy_write` on dst (a self-copy is one combined access);
- a compute `dispatch` ⇒ each bound view's access class: `readonly` ⇒ `shader_read`, `readwrite` ⇒
  `shader_write`, `uniform` ⇒ `uniform_read` (the inferred replacement for a per-binding declaration).

The mapping lives in [access_inference.hh](../../src/shaped-graphics/backend/access_inference.hh) so every backend
agrees on the semantics.

**The one exception — arrays / bindless.** Element usage of a resource *array* bound to a shader cannot be
inferred: the shader may index only some elements, or use them differently. So the caller declares it
explicitly — split by resource family, since buffers have no layout: `declare_array_buffer_access` takes
`array_buffer_access` `{index, stages, access}`, and `declare_array_texture_access` takes
`array_texture_access` which also carries the required `layout` (and, later, a subresource range). (Full
wiring awaits an array binding path + a name→resource reflection map; the buffer API + validation are in
place, the texture path is stubbed until `sg::texture` lands.)

## The vocabulary is backend-neutral

[resource_access.hh](../../src/shaped-graphics/backend/resource_access.hh) defines `access_flags` (what an op
does — `shader_read`, `copy_write`, …), `pipeline_stage_flags` (where — `compute`, `copy`, …), and
`texture_layout` (buffers are always `general`; textures use `shader_readonly` / `shader_readwrite` /
`render_target` / `depth_readonly` / `depth_readwrite` / `copy_src` / `copy_dst` / `present`). These are
deliberately **not** any one backend's spelling; each value documents its D3D12 and Vulkan mapping.
`is_unordered_write` marks the writes that need a hazard barrier (shader/copy/accel writes) — color/depth
*targets* are ROP-ordered freebies.

## Minimal barriers: the three-timeline state

[resource_access_state.hh](../../src/shaped-graphics/backend/resource_access_state.hh) is the reusable state
machine a backend feeds declared accesses into. It keeps three timelines so read-after-read is free and
only the *delta* of new work is synced:

- `curr_*` — what the next op will do (accumulated by `declare`, consumed by `flush`);
- `inflight_*` — everything issued since the last write / command-list start;
- `barriered_read_*` — the reads already synced against the last write.

`flush` compares `curr` against the in-flight state, returns the `access_barrier` to emit (or nothing for
a freebie — a first write, a read with no writer in flight, or a read already barriered), and rolls the
state forward. This building block is **opt-in**: a backend that emits explicit barriers uses it; a
driver-barrier backend (opengl/webgl) ignores it. Emission is entirely the backend's own — there is no
core "emit this barrier" seam.

## Subresources: a covering partition (designed-in for textures)

A texture's subresource domain is the grid (mip × array slice × aspect plane). Buffers are
single-subresource and never touch this. [subresource.hh](../../src/shaped-graphics/backend/subresource.hh)
tracks per-subresource state as a **covering partition**: a set of range-boxes that always exactly tile
the whole domain. Declaring an access to a sub-range *splits* boxes so the range aligns to box boundaries
(keeping the tiling exact), then touches only the covered boxes; `try_merge` collapses back to one box
when every box's state is equal. This is the improvement over the legacy tracker's flat per-subresource
array — range-boxes instead of one entry per subresource. It is now exercised by dx12 textures (see
below); a backend keeps one partition per open command-list slot plus a canonical one.

## Concurrent command lists (the concurrency model)

Every command list is "concurrent": on creation it takes a **slot** from the context's
[command_list_slot_allocator](../../src/shaped-graphics/backend/command_list_slot.hh) (a mutex-guarded 64-bit free
bitmask — lowest clear bit — with a heap free-list past 64, which warns since that many concurrent
recorders usually means a leaked list). The slot keys the list's **private** access-state entry inside
each resource it touches (a `cc::small_vector` of per-slot states, so a few parallel lists don't
allocate). Several lists can therefore record against the same resource at once without sharing state.

Each resource also has a **canonical** state — the shared state between command lists. A list starts a
resource's slot from canonical on first touch, tracks intra-list hazards privately, and each resource
separately counts how many open lists are currently using it. On **submit**, per touched resource:

- if this list's finalize drops that resource's live-user count to **0** (it was the last list using it),
  the list **promotes** its final state to canonical;
- otherwise it **reverts** the resource to the canonical layout it seeded from — stable while other lists
  use it, since a user count ≥ 1 blocks any promote (for a texture this emits the transitions back to that
  layout and warns, because the revert is a hidden cost of concurrent recording).

The decision is **per resource, not global**: one list can be the last user of texture A (and promote it)
while still sharing texture B with another open list (and revert B). Each resource makes the call under its
own mutex. So **only the last-user submit may leave a resource in a new canonical layout**; every other
submit hands it back exactly as it found it. In the fully-serial case (one list open at a time) every
finalize is a last user, so there are no reverts — zero overhead. On **drop**, the recorded work never runs,
so the list just drops each resource's user count and clears its slots (canonical unchanged).

**Buffers skip all of this.** A buffer has no layout, and D3D12 decays it to `COMMON` at
`ExecuteCommandLists`, so cross-list ordering is free and only *intra-list* hazards ever need a barrier. A
buffer therefore keeps **no** canonical state and **no** user count — each list seeds a fresh state and just
clears its slot at submit/drop. The canonical / promote / revert machinery above is a texture concern.

The one *global* requirement is ordering: a submit's finalize writes the canonical layout and its
`ExecuteCommandLists` realizes it, so finalize order must equal execute order. dx12 gets that for free by
running finalize + execute under the one lock that already serializes queue submission + fence signal
(`_next_submission`) — no extra submit-wide lock is needed.

Fine-grained data hazards *between* concurrently-recorded lists on the same resource remain the caller's
responsibility (as in Vulkan): the model orders gross execution by submit order + the epoch fence and
keeps each list's layout bookkeeping consistent, but it cannot see across two lists recording at once.

## dx12: buffer barriers + texture layout transitions

dx12 tracks intra-list hazards with the shared state machine and emits **enhanced barriers**
(`ID3D12GraphicsCommandList7::Barrier`) — see
[dx12_barrier.hh](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_barrier.hh). This replaced the
old stopgap that bounced every buffer through `COMMON` after each copy: uploading then downloading (or
self-copying) the **same** buffer now works in one command list with a precise
`COPY_DEST→COPY_SOURCE`-style transition.

**Access is declared, then flushed, then emitted — batched per operation.** An operation first *declares*
access on every resource it touches — a copy's src + dst, or a dispatch's whole bound group — which only
accumulates into each resource's next-op state (no barrier yet). `flush_barriers()`, just before the op,
then flushes each declared resource (turning its accumulated declares into barriers) and submits the whole
batch in one `Barrier` call (one `D3D12_BARRIER_GROUP` per type). Two payoffs: a dispatch binding many
resources pays one barrier call, not one per binding; and a resource bound *more than once* to the same op
(e.g. two views of one texture) merges its declares into a single barrier carrying the **union** of the
accesses, rather than emitting a redundant barrier per binding.

When those bindings need *different* layouts — a texture bound as both a sampled (`shader_readonly`/SRV) and
a storage (`shader_readwrite`/UAV) view — `combine_layouts` picks one that serves both. No specialized D3D12
layout serves both an SRV and a UAV, so it falls back to `general` (COMMON) and warns once (sampling in
COMMON is slower). A genuinely incompatible pair (e.g. copy-dest + sampled in one op) asserts.

For **buffers specifically the concurrency machinery is teeth-free**: a dx12 buffer's layout is always
`general` (D3D12 decays buffers to `COMMON` at `ExecuteCommandLists`), so revert emits nothing and
cross-list ordering rides on that decay — no trailing barriers.

**Textures give the machinery teeth.** Each `dx12_texture` owns a per-command-list covering partition
([dx12_texture_access](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_texture_access.hh)):
`declare` accumulates the covered subresource boxes' access, and `flush` rolls them through the state
machine and returns the per-box `D3D12_TEXTURE_BARRIER`s (scoped to a `D3D12_BARRIER_SUBRESOURCE_RANGE`,
`LayoutBefore→LayoutAfter`) the command list batches and emits before the op; a non-last-user submit returns
the reverse transitions back to the canonical layout (flushed before `Close`), and warns.
This is dx12-owned end to end — SG core hands out no barriers, only the neutral state machine + partition;
barrier models differ enough across backends (Vulkan image layouts / aspects / queue ownership) that each
owns its tracking + emission. No public op records against a texture yet, so the tracking is wired + tested
but not yet driven by a copy / upload / dispatch. The **vulkan** backend reuses the shared vocabulary +
state machine with its own emission when its compute/transfer milestone lands.

## See also

- [resource_access.hh](../../src/shaped-graphics/backend/resource_access.hh) — the neutral vocabulary.
- [resource_access_state.hh](../../src/shaped-graphics/backend/resource_access_state.hh) — the three-timeline machine.
- [subresource.hh](../../src/shaped-graphics/backend/subresource.hh) — the covering partition.
- [command_list_slot.hh](../../src/shaped-graphics/backend/command_list_slot.hh) — the concurrency substrate.
- [threading](threading.md) — the thread model concurrent recording builds on.
