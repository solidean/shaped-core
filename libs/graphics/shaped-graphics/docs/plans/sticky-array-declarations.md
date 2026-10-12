# Plan: sticky array declarations

Status: **built** — the [decisions](#decisions) record what an independent review settled, and [where it is built](#where-it-is-built) names the code.
An array declaration used to cover one dispatch or draw, so a caller binding a large bindless table for many ops paid O(elements) per op.
This plan makes a declaration live until the caller withdraws it, so sg pays O(elements) once and O(arrays) per op after that.
Background: [barriers](../concepts/barriers.md) for the tracking model and [bindings — array bindings](../concepts/bindings.md#array-bindings) for the contract.

## Scope

The plan's actual scope is three pieces, and nothing else is required for the win:

- **sv declares once per `execute`** — wavefront, raster and megakernel — through the value-typed API below.
- **sg skips a current read-only declaration** on dx12, vulkan and metal, with the invalidation rules below.
- **metal retains its bound groups** rather than copying their contents at every bind, which the skip needs on metal.

Two dx12 prerequisites have landed separately, and the plan builds on them:

- **(P1) one lock per element track.**
  `dx12_buffer::track_access` and its texture twin do declare, pending-barrier and recorded under one lock, as metal does.
- **(P2) transfer waits fold at submit.**
  dx12 folds them in [`fold_transfer_waits`](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc) at submit, before `_next_submission` is taken.
  It folds the upload value of every touched buffer and texture.
  It folds the download value of every buffer the list writes, and of every texture it touches.
  A texture owes that whatever the list does to it, since its entry barrier can change the layout a copy-queue readback is still using.
  vulkan folds at the same point, before its own submission lock ([vulkan_command_list.cc:159-178](../../backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_command_list.cc#L159-L178)).
  The record-time fold is gone from `track_*`.

## Where it is built

- **The value and its lifetime**: [`sg::array_access_declaration`](../../src/shaped-graphics/command_list/array_access_declaration.hh#L28), made by the facades.
  The compute and trace facades make it in [compute.hh](../../src/shaped-graphics/command_list/compute.hh#L81-L91) and [raytracing.hh](../../src/shaped-graphics/command_list/raytracing.hh#L77-L85).
  The raster facades make it in [raster.hh](../../src/shaped-graphics/command_list/raster.hh#L159-L168).
  The core list keeps the links and ends them all when it dies ([command_list.cc:9-40](../../src/shaped-graphics/command_list/command_list.cc#L9-L40)).
  Every backend's submit ends them too, after asserting there are none ([command_list.cc:93-100](../../src/shaped-graphics/command_list/command_list.cc#L93-L100)).
  It ends the raster ones at `close_rendering` ([raster.cc:109-110](../../src/shaped-graphics/command_list/raster.cc#L109-L110)).
- **The backend seam**: the four virtuals return an id, and `withdraw_array_declaration` ends one.
  The compute pair and the withdraw are at [command_list.hh:138-148](../../src/shaped-graphics/command_list/command_list.hh#L138-L148).
  The raster pair is at [command_list.hh:191-196](../../src/shaped-graphics/command_list/command_list.hh#L191-L196).
- **What every backend shares**: [`sg::impl::array_declaration_book`](../../src/shaped-graphics/barrier/array_declarations.hh#L40).
  It holds one bind point's declarations, their per-name generations and the application records.
  Its `apply` is the per-op procedure under [what an op does](#what-an-op-does).
  Its `withdraw` and `end_all` are where a declaration ends, and where the never-met check runs ([array_declarations.cc:44-93](../../src/shaped-graphics/barrier/array_declarations.cc#L44-L93)).
- **The backends** feed it their bound groups and their trackers, each from its own `declare_array_accesses`.
  dx12's is at [dx12_command_list.cc:498](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc#L498).
  vulkan's is at [vulkan_command_list.compute.cc:150](../../backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_command_list.compute.cc#L150).
  metal's is at [metal_command_list.cc:925](../../backends/metal/src/shaped-graphics/backends/metal/metal_command_list.cc#L925).
  Each tracker reports `sg::impl::member_track` from inside the lock its declare takes, and the list counts `member_writes`.
  metal retains every bound group and reads it at each op ([bind_group_to_table](../../backends/metal/src/shaped-graphics/backends/metal/metal_command_list.cc#L742)).
  webgpu returns ids for its empty declarations.
- **sv**: `bound_resources::declare_*_access` return an [`sv::declared_tables`](../../../shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.hh#L134).
  The wavefront makes one per `execute` ([wavefront_routine.cc:268](../../../shaped-viewer/src/shaped-viewer/rendering/wavefront_routine.cc#L268)).
  A raster pass makes one when it has a draw to make ([raster_routine.cc:282](../../../shaped-viewer/src/shaped-viewer/rendering/raster_routine.cc#L282)).
  A trace makes one for itself ([pathtrace_routine.cc:339](../../../shaped-viewer/src/shaped-viewer/rendering/pathtrace_routine.cc#L339)).
- **The count** a test reads is `sg::stat::array_element_tracks`.
- **Tests**: [array-declaration-book-test.cc](../../tests/barrier/array-declaration-book-test.cc) has no device.
  [sticky-array-test.cc](../../tests/barrier/sticky-array-test.cc) runs on every backend with binding arrays.

## The problem

sv's wavefront path tracer binds sv's bindless tables for many dispatches per round: extend, connect, and one shade dispatch per material permutation.
Until this plan was built, its `use` lambda declared every table before each of them:

```cpp
cmd.compute.bind_pipeline(kernel);
cmd.compute.bind_group(0, *traced);
if (second == second_group::bindless)
{
    cmd.compute.bind_group(bindless_group, *t.bindless->group());
    t.bindless->declare_compute_access(cmd);
}
```

`declare_compute_access` rebuilds one element vector per table from everything acquired this epoch, and hands it to sg.
That is `declare_tables` in [gpu_resource_manager.cc:119-142](../../../shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.cc#L119-L142).
On Intel's Sponza (405 meshes, 72 textures) that is on the order of 2000 buffer elements and a couple hundred textures, every time.

What was measured (720p window, upscaled moving frames, `relwithdebinfo`, with other GPU work running on the machine, so indicative rather than exact):

| | |
|---|---|
| wavefront GPU work, moving frame | ~8.5 ms |
| CPU recording, moving frame | ~90–115 ms |
| `trace_layer` alone | ~115 ms |
| CPU recording, still frame | ~9 ms |
| main-thread stack samples in `dispatch_indirect` → `declare_array_accesses` | 10 of 12 |
| ...of those, in `track_buffer_access` | 8 |

A round dispatches extend, connect, and one shade per permutation ([wavefront_routine.cc:303-351](../../../shaped-viewer/src/shaped-viewer/rendering/wavefront_routine.cc#L303-L351)).
That is ~30 bindless dispatches per round, assuming ~28 permutations.
A moving frame drains 22 rounds of those, so ~660 ops times ~2200 elements is ~1.45 M element tracks per frame.
The megakernel does not suffer: it traces once per frame.

## Today, precisely

This section describes sg before P1 and P2, since that is what was measured, and before this plan was built.
Its links are pinned to commit [`78063ef95`](https://github.com/solidean/shaped-core/commit/78063ef950c4528b675a8ee1285450b6043a75c4), the last one before P1 and P2 landed.
The tree that ships this doc has P1, P2 and the plan itself, which moved or removed the code these links cite.

### The API

Every scope that runs shader code carries the pair, documented as "applied to the next dispatch only" ([command_list/compute.hh:80-89](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/src/shaped-graphics/command_list/compute.hh#L80-L89)):

```cpp
void declare_array_buffer_access(cc::string_view binding_name, cc::span<array_buffer_access const> elements);
void declare_array_texture_access(cc::string_view binding_name, cc::span<array_texture_access const> elements);
```

The same pair sits on `command_list_raytracing_scope` ([command_list/raytracing.hh:76-83](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/src/shaped-graphics/command_list/raytracing.hh#L76-L83)).
It sits on all three raster facades too: `rendering_scope`, `command_list_raster_manual_scope` and `command_list_raster_scope`.
Their pairs start at [command_list/raster.hh:158-167](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/src/shaped-graphics/command_list/raster.hh#L158-L167), :228 and :289.
They forward to four backend virtuals, at [command_list/command_list.hh:137-142](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/src/shaped-graphics/command_list/command_list.hh#L137-L142) and :182-187.
There are two bind points, not three: the raytracing scope forwards to the compute virtuals ([command_list/raytracing.cc:133-144](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/src/shaped-graphics/command_list/raytracing.cc#L133-L144)).
That is because DXR binds through the compute root signature.

### The lifecycle of a pending declaration

**dx12.** A declare copies its name and elements into a pending list ([dx12_command_list.cc:443-461](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc#L443-L461)).
That is one heap-allocated `{cc::string, cc::vector}` per call.
Compute and raytracing share `_pending_array_buffer_declares` / `_pending_array_texture_declares` ([dx12_command_list.hh:149-150](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.hh#L149-L150)).
Raster has its own pair (:174-175).
`declare_array_accesses` consumes them at `compute_dispatch` (dx12_command_list.cc:338), `compute_dispatch_indirect` (:357) and `raytracing_dispatch_rays` (:424).
Every draw consumes the raster pair, in `declare_raster_draw_barriers` ([dx12_command_list.raster.cc:382](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.raster.cc#L382)).
`declare_array_accesses` clears both lists at its end (dx12_command_list.cc:603-604), and `raster_end_rendering` clears the raster pair too (raster.cc:208-209).

**vulkan** is the same shape: pending lists at [vulkan_command_list.hh:137-138](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_command_list.hh#L137-L138) and :175-176.
They are consumed at compute.cc:275 and :293, vulkan_raytracing.cc:464 and raster.cc:413.
They are cleared at the end of `declare_array_accesses` and at `raster_end_rendering` (raster.cc:234-235).

**metal** keeps the same four lists ([metal_command_list.hh:480-486](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_command_list.hh#L480-L486)).
They are consumed at metal_command_list.cc:784, :802, :1192 and :1550, and cleared the same way (:1417-1418 for raster).
Its barriers name stages and never a resource, so cover-all costs it one barrier.
It does not hold a bound group: `bind_group_to_table` copies the group's buffers, textures, tlases and array bindings into per-slot vectors.
That copy is [metal_command_list.cc:738-761](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_command_list.cc#L738-L761).
The array copy is O(capacity), two shared pointers per element, at every bind.
It also clears every bound group when a rendering scope begins (:1226-1229).

**webgpu** has no binding arrays: all four virtuals assert that `elements` is empty.
The compute pair is at [webgpu_command_list.compute.cc:163-174](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_command_list.compute.cc#L163-L174).
The raster pair is at [webgpu_command_list.raster.cc:241-252](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_command_list.raster.cc#L241-L252).

What `declare_array_accesses` does per op, on dx12 ([dx12_command_list.cc:487-605](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc#L487-L605)) and identically on vulkan and metal:

1. For every pending declaration, find the *first* bound array of that name and family, and `CC_ASSERT` that each element is in range and not vacant (:496-526).
   That is two checks per element per op in `relwithdebinfo`, and none in `release`.
2. For every array binding of every bound group, gather the pending declarations naming it into an `array_declarations` summary — named, any element, access union (:534-551).
3. Ask `sg::impl::plan_array_declarations` how this op meets them ([barrier/footprint.cc:159-219](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/src/shaped-graphics/barrier/footprint.cc#L159-L219)).
   It answers `skip` when the code never indexes the array, or it was declared empty and the code does not write it.
   It answers `as_declared` with a `widen_by` that adds the code's access where the declaration missed it, logging once.
   It answers `cover_all` when the array was not declared, or was declared empty and is written, logging once.
4. Track each declared element (`as_declared`) or every element (`cover_all`) through `track_buffer_access` / `track_texture_access`.

**An existing bug sits in step 1.**
`find_array_binding` returns the first bound group holding the name, but step 4 indexes the array of *every* bound group holding it.
A second group with a smaller array of the same name is indexed out of range there.
`cc::vector`'s own bounds assert catches that in `relwithdebinfo`, and nothing catches it in `release`.
The per-group check at first meeting, below, fixes it.

### What one element costs

`track_buffer_access` ([dx12_command_list.cc:80-120](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc#L80-L120)), before P1 and P2, does for every element of every op:

- for a read, two atomic loads of the buffer's pending upload values (four for a write), each folded into a per-list wait set (`fold_group_wait`, :44-53);
  the fold returns early when nothing is pending, so in the steady state it is the loads alone;
- three separate locks of the buffer's `cc::mutex<access_tracking>` — `declare_access`, `mark_pending_barrier`, `mark_recorded`;
  they are [dx12_buffer.cc:37-96](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_buffer.cc#L37-L96);
- a push onto `_pending_barrier_buffers`, which `flush_barriers` (dx12_command_list.cc:184-216) then walks;
  it takes the same lock a fourth time in `flush_access` ([dx12_buffer.cc:68-79](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/dx12/src/shaped-graphics/backends/dx12/dx12_buffer.cc#L68-L79)).

P1 makes that two locks, P2 removes the loads, and both stay O(elements) per op.
vulkan has the three-call shape ([vulkan_command_list.cc:423-440](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_command_list.cc#L423-L440)).
It folds no transfer value while recording, but it reads one per tracked texture, `has_pending_transfer()` (:455), as dx12 does (dx12_command_list.cc:159).
metal already fuses the three into one lock ([metal_command_list.cc:205-243](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/backends/metal/src/shaped-graphics/backends/metal/metal_command_list.cc#L205-L243)).

### A repeated read is already free barrier-wise

The cost is CPU work that emits nothing.
[barrier/resource_access_state.hh:143-153](https://github.com/solidean/shaped-core/blob/78063ef950c4528b675a8ee1285450b6043a75c4/libs/graphics/shaped-graphics/src/shaped-graphics/barrier/resource_access_state.hh#L143-L153) flushes a read like this:

```cpp
else if (!curr_read_access.is_empty())
{
    if (!has_inflight_writes())
    {
        // No writer in flight: reads run free, just widen the in-flight read set.
        inflight_read_stages |= curr_read_stages;
        inflight_read_access |= curr_read_access;
    }
    else
    {
        // Sync only the *new* reads (stages/access not already barriered) against the last write.
```

So the second declaration of the same read, at the same stages, with no write in between, produces no barrier and no state change.
**That is the property this plan rests on: skipping such a re-declaration is exactly equivalent to making it.**
A write breaks it: a flush carrying a write clears `barriered_read_*` (:138), so the next read takes the barrier branch (:151-165) — and must not be skipped.

One wrinkle: a layout transition routes the read into the write bucket (`declare`, :82-95), so the op after a transition pays one redundant read-to-read barrier today.
Skipping that re-declaration drops a barrier nothing needed.

## Every caller

These are the callers the change touched, at their lines in the tree that ships this doc.
A test written for the new API, such as `sticky-array-test.cc`, is not among them.

| caller | where | before | after |
|---|---|---|---|
| sv wavefront | [wavefront_routine.cc:268](../../../shaped-viewer/src/shaped-viewer/rendering/wavefront_routine.cc#L268) | in `use`, per bindless dispatch: ~660 declares per moving frame | one declaration before the round loop |
| sv raster | [raster_routine.cc:282](../../../shaped-viewer/src/shaped-viewer/rendering/raster_routine.cc#L282) | one per draw — 405 on Sponza, the same O(elements) each | one declaration per rendering scope, before the draw loop |
| sv megakernel | [pathtrace_routine.cc:339](../../../shaped-viewer/src/shaped-viewer/rendering/pathtrace_routine.cc#L339) | one declare, one trace | the same, holding the result across the trace |
| sv `bound_resources` | [gpu_resource_manager.cc:118-167](../../../shaped-viewer/src/shaped-viewer/resources/gpu_resource_manager.cc#L118-L167) | three `void declare_*_access(cmd)` | return a value that holds the declarations |
| sg tests | `tests/pipeline/dispatch-test.cc:799,807`, `tests/compute/compute-sync-test.cc:100,193` | declare, dispatch | hold the result across the op |
| backend tests | `vulkan-raster-test.cc:381,444`, `metal-vertex-input-test.cc:429-605`, `metal-compute-test.cc:320` | the same | the same |
| dxc tests | `shaped-shader-compiler-dxc/tests/end-to-end-array-bindings-test.cc` | the same, plus lifecycle cases | see [tests](#tests) |

shaped-rendering, the examples and every other library declare nothing.
`plan_array_declarations` is called only from the book's `apply`, which the three backends share, and from `tests/barrier/array-plan-test.cc`.

## The design

### The API

A declaration becomes a value.
It is in force on its bind point for as long as it lives, and destroying it withdraws it:

```cpp
/// One array binding's declared per-element access, in force on the bind point it was made on while this value lives.
/// Move-only: destroying or `withdraw`ing it ends the declaration, and a moved-from one ends nothing.
/// Dropping its list ends it silently; submitting the list asserts that it is gone, and ends it if it is not.
/// A raster declaration also ends when its rendering scope closes.
/// Once ended by any of those, destroying the value does nothing — it never touches a list that is gone.
class sg::array_access_declaration
{
public:
    array_access_declaration() = default;
    array_access_declaration(array_access_declaration&& rhs) noexcept;
    array_access_declaration& operator=(array_access_declaration&& rhs) noexcept;
    ~array_access_declaration();

    /// Ends the declaration now rather than at destruction.
    void withdraw();

private:
    // Shared with the list, which clears it when the declaration ends from its side — scope close, submit, drop.
    // The value reaches the list only through a live token.
    impl::array_declaration_token _token;
};
```

On every scope that has the pair today, it is replaced by:

```cpp
/// Declares per-element access for a *buffer* array / bindless binding, for every dispatch on this bind point while the result lives.
/// Array element usage cannot be inferred, since a shader may index only some elements, or use them differently.
/// Every live declaration naming an array applies to it, and an empty `elements` declares it unused.
/// Elements are resolved against whichever bound group holds the array at each op, and checked the first time this declaration meets that group.
[[nodiscard]] array_access_declaration declare_array_buffer_access(cc::string_view binding_name,
                                                                   cc::span<array_buffer_access const> elements);

/// The texture form: each element also names the layout it must be in.
[[nodiscard]] array_access_declaration declare_array_texture_access(cc::string_view binding_name,
                                                                    cc::span<array_texture_access const> elements);
```

The raster forms say "every draw" and still require an open rendering scope.
The backend virtuals return the id the token carries, and one new virtual ends it:

```cpp
virtual u32 compute_declare_array_buffer_access(cc::string_view binding_name, cc::span<array_buffer_access const> elements) = 0;
virtual u32 compute_declare_array_texture_access(cc::string_view binding_name, cc::span<array_texture_access const> elements) = 0;
virtual u32 raster_declare_array_buffer_access(cc::string_view binding_name, cc::span<array_buffer_access const> elements) = 0;
virtual u32 raster_declare_array_texture_access(cc::string_view binding_name, cc::span<array_texture_access const> elements) = 0;

// Ends declaration `id`, which is live: the token guarantees it.
virtual void withdraw_array_declaration(u32 id) = 0;
```

**Why a value rather than "until replaced".**
A declaration that outlives its routine leaks into the next one recording on the same list.
sv shows how that goes wrong: `freeze()` hands back the *same* cached group when nothing new was minted, while `acquired` may have grown.
A later routine that binds that group and forgets to declare would inherit a declaration that misses elements — silently, where today it logs and covers the array.
Tying the lifetime to a value keeps "forgetting is loud": once the value dies, the array is undeclared again.
`rendering_scope` is the precedent for a recording guard tied to a list.
webgpu asserts "a rendering scope is still open at submit" ([webgpu_command_list.cc:83](../../backends/webgpu/src/shaped-graphics/backends/webgpu/webgpu_command_list.cc#L83)).

### When a declaration is in force

- **From the call until the value is destroyed or withdrawn.**
- **A raster declaration also ends when its rendering scope closes**, which keeps today's rule that a raster declaration goes with its scope.
- **Submit ends every live declaration, as dropping the list does.**
  Submitting with one alive is still a bug, and submit asserts it.
  Ending them as well means a `release` build, with that assert compiled out, never leaves a token naming a submitted list.
- **Dropping the list ends every declaration silently.**
  A drop is what exception unwinding does to a half-recorded list, and a value destroyed later must find its token dead rather than a freed list.
- **Binding a pipeline or a group does not end it.**
  This is load-bearing: dx12 and vulkan reset every bound group when a pipeline is bound.
  dx12 does it at [dx12_command_list.cc:265](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc#L265).
  vulkan does it at [vulkan_command_list.compute.cc:36](../../backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_command_list.compute.cc#L36).
  The wavefront rebinds the pipeline and the bindless group before every dispatch, so a declaration that ended on a rebind would end at every op.
- **An op where no bound group holds the array leaves the declaration dormant**, rather than asserting as today.
  The wavefront's segment, sort and argument kernels bind no bindless group, and its declaration has to survive them.
- **A declaration that never met its array in its whole lifetime asserts when it ends.**
  That is where the misspelled name is caught, since a dormant declaration is otherwise legitimate.
  "Met" means some op had a bound group holding an array of that name and family.
  The check runs at withdraw, at destruction, and when a rendering scope's close ends a raster declaration.
  The scope close matters most: raster declarations are scoped, so a misspelled raster name would otherwise never be reported.
  A submit and a list drop end a declaration without the check.
- **The never-met check does not run while an exception unwinds.**
  A declaration is made after its list, so unwinding destroys it first, and the check would turn the exception into an abort.
  Each declaration records `std::uncaught_exceptions()` when it is made, and its end skips the check when that count has grown since.
- **webgpu never runs the never-met check.**
  It has no binding arrays, so no declaration there can meet one, and an empty declaration must still work as it does everywhere else.
- **Compute and raytracing stay one bind point**, as they are on every backend today.

A declaration is keyed by name and family, never by bind event.
Every live declaration naming an array applies to it, unioned exactly as today's `gather` unions several declares before one op.

### What an op does

At each dispatch, trace or draw, for each array binding of each bound group:

1. **Plan.** Summarize the live declarations naming the array — named, any element, access union — and call `plan_array_declarations` exactly as today.
   The summary is kept up to date at declare and withdraw time, so it costs O(1) here.
   `skip` and `cover_all` behave exactly as today, per op.
2. **Resolve on first meeting.** The first time a declaration meets a particular group, check every element's range and vacancy against *that* group's array.
   The check is per declaration, not per name: one made after its name already met a group is still checked against that group, at its next op there.
   Each index is resolved to its resource then.
   Checking per group is what fixes the existing first-group-only bug.
   A different group at the same name resolves and checks again.
3. **Apply, or skip.** An `as_declared` read-only array is skipped iff it is [current](#a-write-between-two-reads) and its key is covered.
   Otherwise every element is tracked — and always for a declaration that writes, declared or widened by `widen_by`, as today.
   No caller holds a writable table across ops, so stickiness never needs to cover that case.

**The key.**
An array's application record holds the group's identity, the generation of its name and family, and the texture layouts it applied.
It also holds the stages and access applied since its last invalidation, as a union.
The generation is per name and family: declaring or withdrawing a declaration of `tables.textures_2d` bumps that array's alone, so a one-op declaration of another name leaves this one current.
The key is **covered** when the group and the generation match, the op's stages and access are within the recorded union, and every layout matches exactly.
Covered rather than equal is sound because a read at stages already synced since the last write needs no barrier.
It also absorbs the compute ↔ raytracing switch, which changes `op_stages` on the shared bind point, and the raster routine alternating fragment-only and vertex-plus-fragment pipelines.
An invalidation clears the union.

**Group identity.** dx12 and vulkan hold every bound group until the list is consumed (`keep_bound`), so the group's address is a sound identity there.
metal must retain the group too; see [per backend](#per-backend).

### A write between two reads

Skipping is only equivalent to declaring while nothing else changed the element's state.
A read by another op cannot hurt: it widens the in-flight reads, and the skipped re-declaration would have been free.
What can hurt is a **write or a layout change** — then the next reader needs a barrier the skip would drop.

**One counter per list, bumped on every write-bucket hit.**
Each resource's per-slot tracking state gains a flag, `array_member`, set when an application tracks it.
It lives behind the one lock P1's `track_access` takes, so reading it costs nothing extra.
Any tracked access to a member that lands in the write bucket — an unordered write, or a layout change — bumps the list's `member_writes`.
That covers every path: a scalar binding, a copy, an upload, a render target, a native scope, and **an application's own layout transitions**.
The counter is per list rather than per bind point, because a compute declaration and a raster one can hold the same resource.
There is no exemption for an application's own transition.
Exempting it is unsound: declaration A applies texture T at `shader_texture`, declaration B applies T at `shader_image`, and A's later skip leaves T in B's layout.
The same goes for a raster and a compute declaration sharing T, and for groups alternating A, B, A at one name.

**Current means: nothing written since my snapshot, and nothing before it in my op.**
An application snapshots `member_writes` right after its own tracking.
It is current iff `member_writes` at the end of the op still equals that snapshot, and stays current while it does.
A member written later in the same op — a vertex buffer, another array's application — moves the counter past the snapshot.

A member written *earlier* in the same op, before the application, does not.
`declare_group_accesses` runs before `declare_array_accesses` ([dx12_command_list.cc:340-343](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc#L340-L343)).
So at application `track_access` also reports whether the slot's current-op state already holds a write (`has_curr_writes()`) or a pending layout change.
Either makes the application not current.
Without that, a scalar write to element `e1` in the op that first applies the array would leave it current, and the next op would skip `e1`'s read-after-write barrier.

The counter is list-wide and invalidates every declaration, which is coarse but cheap and obviously right.
A member write inside a run of read-only dispatches does not happen in any caller today: every bindless element is written before the trace starts.
Re-tracking only the written members is the refinement if that ever changes.

### The footprint rules, per op

The rules in [barriers.md](../concepts/barriers.md) are unchanged in what they decide; what changes is that one declaration meets many ops.

- **An op whose pipeline does not index the array** plans `skip`: nothing is tracked, the element states are untouched, and the declaration stays current.
- **An op whose code writes an array declared read-only** plans `as_declared` with `widen_by`, logs once per pipeline as today, and is applied in full — a write is never skipped.
- **An op indexing an array with no live declaration** logs and covers all, as today.
- **An empty live declaration** says "unused" for every op it lives through, not just one.
  A pipeline that reads the array anyway gets no barrier and no log — today's rule, over a longer window.
- **A hot reload** changes the pipeline, so its stages or `widen_by` may fall outside the key, which re-applies.

### Record-time reads that remain

P2 moved dx12's transfer waits to submit, so a transfer enqueued into a member after its application still makes the list wait.
One record-time read is left: a texture with a transfer in flight is tracked in `general` rather than its declared layout.
That is `has_pending_transfer()` in dx12's `track_texture_access` ([dx12_command_list.cc:127](../../backends/dx12/src/shaped-graphics/backends/dx12/dx12_command_list.cc#L127)).
vulkan's texture track reads it the same way ([vulkan_command_list.cc:471](../../backends/vulkan/src/shaped-graphics/backends/vulkan/vulkan_command_list.cc#L471)).
Sticky freezes that choice at application, where today the next re-declaring op re-reads it.
A transfer that finishes mid-list leaves the texture in `general`, which serves the read.
A transfer enqueued mid-list into a member texture leaves its declared layout in place, where today a later op would move it to `general`.
[barriers.md](../concepts/barriers.md#what-orders-what-the-calls-on-the-context) already calls any such record-time read a defect, and this is the one the plan leaves standing.

### Per backend

**dx12, vulkan, metal.**
Replace the four pending lists with two lists of live declarations, one per bind point.
Each entry holds the token's id, the name, the family, the copied elements, its summary, whether it writes, and whether it has met its array.
Each bind point keeps a per-(name, family) generation and a small application cache, one record per (group, array binding) as described under [the key](#what-an-op-does).
The list keeps `member_writes`.
Withdraw erases the entry, bumps its name's generation, and runs the never-met check.
`raster_end_rendering` ends the raster entries, running the never-met check on each.
Submit asserts both lists empty and then ends what is left, and a drop ends them silently; neither runs the check.
No end runs the check while an exception unwinds.
`track_access` gains a "from an array application" argument and returns whether it hit a member in the write bucket, and whether the slot's current op already held a write or layout change.

**metal** additionally retains each bound group, as dx12 does with `group.weak_from_this().lock()`, and reads its arrays through it.
That makes the group address a sound identity, since a retained group cannot be freed and a new transient allocated at its address within the list.
It also removes the O(capacity) array copy at every bind, which would otherwise cost the wavefront what the skip saves, since it rebinds per dispatch.
The scalar buffer, texture and tlas copies can move the same way; the arrays are the part this plan needs.
Clearing bound groups at rendering-scope begin stays: it ends nothing, since bind events never do.
metal counts each barrier in `metal_command_list::barriers_emitted` and in the context's `global_barriers` and `barrier_calls`.
An MTL4 barrier names no resource, so metal counts no `buffer_barriers`, `texture_barriers` or `texture_transitions`, and its tests read `barriers_emitted`.

**webgpu.** Has no arrays: the virtuals assert empty spans as today, and return an id so the value works.
Its withdraw keeps nothing, so the never-met check never runs there, and an empty declaration on webgpu never asserts.

**The core.** `array_access_declaration`, its token, the facades returning it, and `plan_array_declarations` unchanged.

### What it costs

Per op, for an array whose declaration is current: one summary read, one `plan_array_declarations` call, one key compare.
That is O(bound arrays × live declarations) — sv binds eight tables — and no element is touched.
The O(elements) work happens once per array per list, again after any member write, and again when a key is not covered.

Back of the envelope for a moving Sponza frame:

| | today | P1 + P2 alone | P1 + P2 + sticky |
|---|---|---|---|
| element tracks | ~660 ops × ~2200 = ~1.45 M | ~1.45 M | ~2200, once |
| locks per element track (dx12) | 4 | 2 | 2 |
| per-op array work | O(elements) | O(elements) | O(arrays) |
| `CC_ASSERT` element checks (`relwithdebinfo` only) | ~2.9 M | ~2.9 M | ~4400 |
| heap allocations for pending declares | ~660 × 8 | ~660 × 8 | 8 |

The 90–115 ms was measured in `relwithdebinfo`, so part of it is the assert loops, which `release` compiles out.
At the ~70 ns per element track that figure implies, the array share drops from ~100 ms to well under 1 ms.

What remains per op is not zero, and nothing here measures it.
Each bindless dispatch still pushes the pipeline and groups through `keep_bound`, tracks the scalar `traced` group, sets inline constants and records a timestamp.
An estimate of a few ms per moving frame for those ~660 ops is plausible; whether a moving frame then records near a still one is for a measurement to say.
The raster routine gains the same: 405 draws × ~2200 elements becomes one application per pass.

### Why not detect "same span as last time"

With an explicit lifetime, sg never has to guess that two declarations are the same one.
The invalidation machinery above is needed whichever way sameness is detected — it is the hard part, and the API decides only who says "nothing changed".

## Worked examples

### The wavefront round loop

Before — every bindless dispatch rebuilds and re-declares every table:

```cpp
auto const use = [&](sg::compute_pipeline const& kernel, second_group second)
{
    cmd.compute.bind_pipeline(kernel);
    cmd.compute.bind_group(0, *traced);
    if (second == second_group::bindless)
    {
        cmd.compute.bind_group(bindless_group, *t.bindless->group());
        t.bindless->declare_compute_access(cmd);
    }
    // ...
};
```

After — declared once, before the first dispatch, and held to the end of `execute`:

```cpp
// Every table stays declared for each dispatch that binds them, until `declared` dies at the end of execute.
auto const declared = t.bindless->declare_compute_access(cmd);

auto const use = [&](sg::compute_pipeline const& kernel, second_group second)
{
    cmd.compute.bind_pipeline(kernel);
    cmd.compute.bind_group(0, *traced);
    if (second == second_group::bindless)
        cmd.compute.bind_group(bindless_group, *t.bindless->group());
    // ...
};
```

`sv::bound_resources::declare_*_access` return an `sv::declared_tables`, a move-only holder of one `sg::array_access_declaration` per table.
The first extend dispatch applies them all, and orders the reads after the parameter-block uploads `describe_instance` recorded on the same list.
Every later bindless dispatch finds them current and touches no element.
The kernels that bind no bindless group leave them dormant.

### The raster routine's per-draw declares

Before — inside the draw loop, as sv had it until this plan was built:

```cpp
for (auto const& draw : d.draws)
{
    pass.bind_pipeline(*pipelines[draw.permutation]);
    pass.bind_group(0, *drawn);
    pass.bind_group(bindless_group, *d.bindless->group());
    pass.bind_group(2, *tables);
    pass.set_inline_constants(raster::draw{.instance = draw.instance, .pad0 = {}}.to_block());
    d.bindless->declare_raster_access(cmd);
    pass.draw({.vertex_range = {.offset = 0, .size = isize(draw.triangle_count) * 3}});
}
```

After — once per pass, declared after `pass` so it dies first:

```cpp
auto const declared = d.bindless->declare_raster_access(cmd);
for (auto const& draw : d.draws)
{
    // ...binds and inline constants as before...
    pass.draw({.vertex_range = {.offset = 0, .size = isize(draw.triangle_count) * 3}});
}
```

A pass whose draws list is empty never meets the tables, and the never-met check would assert at the end of `execute`.
sv declares only when there is a draw to make ([raster_routine.cc:279-282](../../../shaped-viewer/src/shaped-viewer/rendering/raster_routine.cc#L279-L282)).
Pipelines touching the tables at different stages re-apply once each until the recorded union covers them both, then skip.
The environment draw binds no bindless group, so the declaration is dormant there.

### The megakernel's single trace

```cpp
cmd.raytracing.bind_group(bindless_group, *d.bindless->group());
cmd.raytracing.bind_group(2, *tables);
auto const declared = d.bindless->declare_raytracing_access(cmd);
cmd.raytracing.dispatch_rays(*variant->table, sg::raygen_index(0), d.output.width(), d.output.height());
```

One application, one trace: the cost is today's, and only the spelling changes.

### A write between two reads, worked through

Three buffers `e0`, `e1`, `e2` sit in array `tables.buffers`, and `e1` is also bound as a scalar `readwrite` binding of a group `patch`:

```cpp
auto const declared = cmd.compute.declare_array_buffer_access("tables.buffers", reads_of_e0_e1_e2);

cmd.compute.bind_pipeline(*reader);   // indexes tables.buffers
cmd.compute.bind_group(0, *tables);
cmd.compute.dispatch_groups(n);       // op 1

cmd.compute.bind_pipeline(*patcher);  // writes e1 through `patch`, indexes no array
cmd.compute.bind_group(0, *patch);
cmd.compute.dispatch_groups(n);       // op 2

cmd.compute.bind_pipeline(*reader);
cmd.compute.bind_group(0, *tables);
cmd.compute.dispatch_groups(n);       // op 3
cmd.compute.dispatch_groups(n);       // op 4
```

| op | what happens | barriers |
|---|---|---|
| 1 | first meeting: resolve and check; apply — `e0..e2` read at compute and become members; snapshot `member_writes = 0`, unchanged at op end, so current | whatever the list's earlier writes need |
| 2 | the scalar write tracks `e1`, a member, in the write bucket → `member_writes = 1`; the array is not bound | `e1`: read → write (write-after-read) |
| 3 | `member_writes` moved past the snapshot, so not current: apply in full — `e1`'s read meets an in-flight write; snapshot 1, unchanged at op end, so current | `e1`: write → read (read-after-write) |
| 4 | current and the key is covered: skip, nothing tracked | none |

Op 4 skipping is exactly what re-declaring would do: `e1`'s read was barriered in op 3, and `e0`, `e2` have no writer in flight.

Now let `patcher` also index `tables.buffers`, and let op 2 be the *first* op to apply it.
`declare_group_accesses` tracks the scalar write to `e1` first, bumping `member_writes` before the application — so the snapshot alone would miss it.
The application's `track_access` on `e1` reports that the op already holds a write, so the application is not current.
Op 3 then re-applies and gets the read-after-write barrier, which is what flush clearing `barriered_read_*` after a write asks for.

## Consequences

### The API break

Every declare call changes from a statement to a value, and `[[nodiscard]]` is what keeps one from dying on the line it was made.
A call written as today's statement would declare and withdraw at once — and then assert as never met; the warning catches it first.
Who changes: sv's `bound_resources` and its three call sites, the sg, vulkan, metal and dxc tests listed under [every caller](#every-caller), and the backends' virtuals.
No shim keeps the old per-op form: a caller that wants one op writes `{ auto const d = ...; dispatch; }`.

### Correctness risks

- **A missed invalidation is a silent race.**
  Every path that tracks a resource must report member writes, and a backend that adds a new tracking path must too.
  The tests below pin it per path; the debug layers and synchronization validation catch the rest.
- **A declaration that meets its array only on some frames** asserts as never met on the others; its owner must declare only where it will meet it.
- **The `has_pending_transfer()` layout choice is frozen at application** — see [record-time reads that remain](#record-time-reads-that-remain).
- **metal's group identity rests on retaining the group**; a backend that copies group contents instead must key on a group serial.

### Tests

New tests, tier 1 (run against every backend that has binding arrays) unless noted:

- **A current declaration tracks no element.**
  Needs a counter: a new `sg::stat::array_element_tracks`, counting element accesses an application feeds the tracker.
  Two dispatches, one declaration: the second adds zero.
- **The worked hazard example**, per path: a scalar write, a copy into an element, an upload, and a texture element moved to `copy_src` by a download.
  Each checks the readback and that the barrier count grew at op 3 and not at op 4 — `buffer_barriers` / `texture_barriers`, and `barriers_emitted` on metal.
- **First-application same-op write**: the op that first applies the array also writes `e1` through a scalar binding; the next op re-applies and barriers `e1`.
- **Two declarations at different layouts** holding one texture — on one bind point, across raster and compute, and as the A, B, A group alternation at one name.
  Each skip must leave the texture in the layout its op needs.
- **A compute declaration alive across a rendering scope that writes a member** re-applies at the next dispatch.
- **metal: a transient group freed and a new one allocated at the same address** is a new identity, resolved and checked again.
- **A one-op declaration of another name** leaves the sticky array current: no element tracks at the next op.
- **An upload enqueued into a member after its application, before submit**, still makes the list wait, on dx12 (P2).
- **The first-meeting check in `release`**: it runs on the release presets too, or the test documents that it compiles out with the other asserts.
- **Pipeline and group rebinds keep it**: the wavefront's bind pattern, asserting zero element tracks after the first op.
- **Covered keys skip**: compute then raytracing on one bind point, and two raster pipelines at different stages, re-apply once each and then skip.
- **A different group at the same name** resolves and checks again: an out-of-range index for the second group asserts at its first op.
- **A second declaration on a name that already met a group** is checked against that group: an out-of-range index in it asserts at its next op there.
  That one needs no device, and is in `array-declaration-book-test.cc`.
- **Two bound groups holding one name at different sizes**: an index valid only in the larger asserts — the first-group-only bug.
- **Withdraw makes the array undeclared**: the next op logs "declared no access" and covers it.
- **A declaration that never met its array asserts at withdraw**, and a dropped list ends one silently.
- **Scope close ends a raster one**, and destroying the value afterwards does nothing.
- **A raster declaration that never met its array asserts when its scope closes**, and the list is whole after.
- **Unwinding past a declaration that never met its array does not assert**, for a value the unwinding destroys and for a scope it closes.
- **Submit with a live declaration asserts, and ends it**: destroying the value afterwards does nothing, in `release` too.
- **webgpu only: an empty declaration ends without asserting**, withdrawn or ended by its scope.

Existing tests that change:

- `tests/pipeline/dispatch-test.cc` "a group bound for a dispatch is not bound at a later draw" probed with the per-op assert at the draw.
  That assert read "declare_array_buffer_access names no buffer array binding of a bound group".
  It becomes the end-of-life never-met assert, so the probe is the raster declaration's withdraw asserting.
- `end-to-end-array-bindings-test.cc` "a draw resolves only its own bind point's declarations" keeps its cases, with the declarations held as values.
  "A declaration still pending when its scope closes goes with the scope" becomes "a raster declaration ends with its scope".
  The declaration that case ends never meets its array, so the close that ends it asserts, and the case expects that.
  "A compute declaration waits for a dispatch" becomes "a compute declaration is never met by a draw".
- The undeclared-array tests (dxc :205, :790, `vulkan-raster-test.cc:407`) keep their `nx::exactly(...)` counts.
- The out-of-range test in `metal-vertex-input-test.cc:437` still asserts at the draw, since the check runs at first meeting.
- `tests/barrier/array-plan-test.cc` is unchanged: `plan_array_declarations` is.

### Docs to update

- [barriers.md](../concepts/barriers.md), the arrays paragraph: "applies to the next dispatch or draw only" becomes the lifetime above, plus the member-write rule.
- barriers.md, "What orders what": dx12 now folds transfer waits at submit (P2), as vulkan does.
- [bindings.md](../concepts/bindings.md#array-bindings): the "Access is never inferred" and "Every array ... before each dispatch or draw" bullets.
- The metal backend's docs: it retains bound groups rather than copying their contents.
- The sg [cheat sheet](../../cheat-sheet.md) lines for `cmd.compute` and `cmd.raster` declarations, and `sg::array_access_declaration`.
- sv's [cheat sheet](../../../shaped-viewer/cheat-sheet.md) (`bound.declare_raytracing_access`) and the `bound_resources` doc comments.
- [tier1-pipeline-tests.md](../tier1-pipeline-tests.md), whose follow-up list mentions skipping `declare_array_texture_access`.

## Alternatives considered

### sg dedupes repeated identical declarations

Keep the per-op API, and have sg notice that this op's declaration equals the last one applied to the same group.

- **For:** no API break, no lifetime to get wrong, and "forgetting is loud" stays per op.
- **Against:** the caller still rebuilds its spans every op, and sg still copies and compares them — O(elements) per op, a memcmp of ~20 KB, a few µs.
  That is cheap next to today, but it is not O(arrays), and a span whose order shifts silently falls off the fast path.
- **The same hard part:** skipping still needs the member-write invalidation, so it is no simpler inside sg.

### Make each element track cheaper

P1 and P2 are this alternative's cheap half, and they landed first.
The rest — running the range and vacancy asserts once per group, dropping the per-declare heap copies — is part of this plan.
Each is a constant factor on an O(elements × ops) term: even halving the per-element cost leaves tens of ms on a moving Sponza frame.
It still matters for the first application, every `cover_all`, every writing declaration, and every scalar binding.

## Decisions

- **A value type, `[[nodiscard]]`** — not `void` "until replaced", whose leak into the next routine is silent.
- **No [dedupe inside sg](#sg-dedupes-repeated-identical-declarations)** — its invalidation is the same work, and it keeps O(elements) per op.
- **Covered by, not equal** — the applied stages and access are a union since the last invalidation; texture layouts must still match exactly.
- **The typo assert is kept, moved to end of life** — a declaration that never met its array asserts at withdraw, destruction or scope close.
- **The lifecycle's edge cases are settled** — submit ends declarations, unwinding and webgpu skip the never-met check, and first meeting is per declaration.
- **A different group at the same name is resolved again**, per group, rather than ending the declaration.
- **Transfer waits fold at submit** — a prerequisite, landed as P2.
- **Lock fusion first and separately** — landed as P1.
