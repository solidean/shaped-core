# Concept: command recording

## What a command list is

[`sg::command_list`](../../src/shaped-graphics/command_list/command_list.hh) is the one object GPU work is recorded into.
`ctx.create_command_list()` returns one already recording, as a `std::unique_ptr`, and everything that records into it takes a `command_list&`.
Submitting and dropping are the exception: both consume the `unique_ptr`, which is what makes "exactly once" a type-level fact.
There is no begin/end pair and no reset — a list records once and is then consumed.

It is **single-use and single-threaded**: recorded by one thread, then submitted or dropped exactly once.

```cpp
auto cmd = ctx.create_command_list();
cmd->upload.data_to_buffer(vertex_buffer, vertices);
auto pass = cmd->raster.render_to({.color_targets = {rtv.cleared(clear)}});
pass.bind_pipeline(pso);
pass.draw({.vertex_range = {.offset = 0, .size = 3}});
ctx.submit_command_list(cc::move(cmd));   // or ctx.drop_command_list(...)
```

Submitting returns an [`sg::submission_token`](../../src/shaped-graphics/fwd.hh), which answers "is *this one* list done?" through `ctx.is_submission_complete`.
Dropping discards the recording without executing it.

## Submit or drop, in the epoch the list was opened in

Both calls sit on the context, and consuming the `unique_ptr` is what makes "exactly once" a type-level fact rather than a convention.

A list **must not span epochs**: `advance_epoch` requires every list opened in the closing epoch to be already submitted or dropped.
That is what lets a backend recycle a whole epoch's command allocators in one step — see [epochs](epochs.md).

Letting a list go out of scope un-consumed **auto-drops it and prints a warning**.
It is a safety net that keeps the open-list count, the access-tracking slot and the allocator from leaking, not a supported path.

## The seven recording scopes

A command list has no flat command surface.
Every operation is reached through one of seven scope members, so the call site names the kind of work it is recording:

| scope | records | owner doc |
|---|---|---|
| `cmd.upload` | host→device writes, visible to later commands in the same list | [inline upload](upload.inline.md) |
| `cmd.download` | device→host reads, delivered after the list runs | [inline download](download.inline.md) |
| `cmd.copy` | device→device buffer copies | — |
| `cmd.compute` | bind a compute pipeline + groups, dispatch | [bindings](bindings.md), [caches](caches.md) |
| `cmd.raster` | open a rendering scope over a set of targets, then draw | [raster pipeline](raster-pipeline.md) |
| `cmd.raytracing` | build acceleration structures, trace rays | [acceleration structures](acceleration-structures.md), [raytracing pipeline](raytracing-pipeline.md) |
| `cmd.query` | GPU timestamps | [GPU queries](queries.md) |

`ctx.upload` and `ctx.download` are the *async* counterparts of the first two — a dedicated copy queue, off the frame path, recorded on no list at all.
See [async upload](upload.async.md) and [async download](download.async.md) for when to use which.

**Every scope is a facade pinned to its list.**
It stores only a back-reference and forwards each operation to a protected virtual on the list, which is the seam a backend implements.
So a scope is neither copyable nor movable, only its own command list constructs it, and it has no lifetime of its own — `cmd.compute` is a member, not a handle to hold.
Where a rule below is per-scope, it is documented on that scope; this one is not repeated.

## Ordering, and what you do not have to declare

Commands execute in the order they were recorded.
Access is **inferred** from each operation rather than declared — an upload implies `copy_write`, a dispatch implies its bound views' access — and the barriers that ordering needs are emitted for you.
Uploading, downloading and copying the *same* buffer in one list therefore works.

One thing is not inferable, and it is not yet wired either.
Per-element access for an array / bindless binding cannot be derived from the shader, since it may index only some elements or use them differently.
`cmd.compute.declare_array_*_access` exists to declare it — but no backend applies the declaration today, so see the gaps below before relying on it.
[barriers](barriers.md) owns the state machine, and its slot model is why several lists may record against the same resource concurrently.

## Rendering is the one nested scope

`cmd.raster.render_to(info)` returns an [`sg::rendering_scope`](../../src/shaped-graphics/command_list/raster.hh) — an RAII handle that begins rendering on construction and ends it at scope exit.
Draws are valid only while one is open.

The draw calls exist in three places and all forward to the same list: on the returned `rendering_scope`, on `cmd.raster`, and on `cmd.raster.manual`.
Prefer the scope handle — it keeps "draw into this pass" on the object that opened the pass, and a routine handed only the scope can record without a separate `command_list` argument.
`cmd.raster.manual.begin_rendering` / `.end_rendering` are the by-hand pair, and must balance.

A rendering scope mirrors no other scope.
Anything else the list offers is reached through `pass.command_list()`.

## Results that outlive the list

A download and a timestamp both hand back a value recorded *now* and readable *later*:
[`sg::bytes_future`](../../src/shaped-graphics/bytes_future.hh) and [`sg::gpu_timestamp`](../../src/shaped-graphics/query/gpu_timestamp.hh).

Neither is readable before the recording list is submitted.
Blocking on one before that would deadlock the thread that has to submit it, so the blocking waits (`ctx.wait_for`, `ctx.wait_for_ticks`) report that case instead of waiting.
Both survive their command list: the future pins its own destination bytes.

An epoch advance does **not** deliver a download — it drains the GPU, and the readback copy runs on a separate actor.

## Load-bearing invariants

1. **A list is consumed exactly once**, by submit or by drop, both through the context.
2. **A list cannot span epochs** — enforced per list, and in aggregate at `advance_epoch`.
3. **Recording is single-threaded per list**; concurrent lists are fine and each takes its own access-tracking slot.
4. **Access is inferred, never declared.** The one exception, array/bindless elements, has an API and no implementation behind it.
5. **A scope is pinned to its list** — no copy, no move, no independent lifetime.
6. **Draws require an open rendering scope**, and `begin_rendering` / `end_rendering` must balance.

## What is implemented today vs deferred

**dx12** records all seven scopes.
**vulkan** creates devices and resources, but its recording paths are stubs.
Only `raytracing.is_supported()` and `query.is_supported()` answer honestly, returning false; every other recording call is a `CC_UNREACHABLE` that aborts rather than a no-op.

Two gaps in the dx12 path are worth knowing before you rely on them.
`cmd.copy` is buffer-only; texture copies land under the same `<resource>_<bytes|data>_region` naming.
`cmd.compute.declare_array_*_access` validates its arguments and then does nothing.
The buffer form drops the declaration; the texture form **asserts on a non-empty one**, since silently ignoring a required layout would leave the texture wrong.
Applying either needs an array binding path plus a binding-name→resource reflection map; [TODO.md](../TODO.md) tracks both.

## See also

- [context](context.md) — where `create_command_list`, submit, drop and the epoch surface live.
- [epochs](epochs.md) — why a list is bound to the epoch it was opened in.
- [barriers](barriers.md) — the access-tracking state machine behind the inferred barriers, and the concurrent-list slot model.
- [cheat-sheet](../../cheat-sheet.md) — every recording call at a glance.
