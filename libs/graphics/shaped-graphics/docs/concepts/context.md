# Concept: context

`sg::context` is the mutable entry point to a graphics backend, and the factory for everything else in sg.
Command lists, buffers, textures, layouts, pipelines and swapchains are all created through it, and none of them may outlive it.

The type is abstract.
A backend subclasses it — `sg::backend::dx12::dx12_context`, `sg::backend::vulkan::vulkan_context` — and you obtain one from that backend's factory:

```cpp
#include <shaped-graphics/backends/dx12/dx12_context.hh>

auto ctx = sg::create_dx12_context({.enable_debug_layer = true}).value();  // -> cc::result<sg::context_handle>
```

`sg::context_handle` is `std::shared_ptr<sg::context>`.

## There is no `sg::create_context`

sg does not depend on its backends; the arrow points the other way.
So the core has no factory that could name one, and each backend library exposes its own `sg::create_<backend>_context(config)` with its own config type.

`ctx.backend()` returns a coarse `backend_kind` tag for the same reason: it identifies the API family, never the concrete class.
Code that branches on it is usually code that should have asked a capability question instead.

The capability questions the context answers directly are `accepted_shader_formats()` / `accepts_shader_format()` (see [shaders](../shaders.md)) and `threading()` (see [threading](threading.md)).

## The scopes: where each create lives

Creation is grouped into scopes that name the *lifetime* or the *transfer direction* of what they hand back, so the choice is made at the call site rather than buried in an argument.

| Scope | What it creates | Reclaimed by |
|---|---|---|
| `ctx.persistent` | long-lived buffers, textures, memory heaps, binding groups | refcount, then the epoch that saw it drop ([epochs](epochs.md)) |
| `ctx.transient` | per-frame scratch buffers, textures and binding groups | the next `advance_epoch` — using one past its epoch asserts |
| `ctx.upload` | *no resources* — streams host bytes into a buffer or texture | n/a ([async upload](upload.async.md)) |
| `ctx.download` | *no resources* — streams device bytes back, returning a future | n/a ([async download](download.async.md)) |
| `ctx.uncached` | binding-group layouts, pipeline layouts, every pipeline kind, shader tables — freshly built every call | refcount |
| `ctx.cached` | layouts plus compute and raytracing pipelines, deduplicated get-or-create | refcount ([caches](caches.md)) |

Two rules cover almost every choice.
**Prefer `ctx.transient` for anything sized by the current frame**, and `ctx.persistent` only for what genuinely outlives it.
**Prefer `ctx.cached` over `ctx.uncached`** — `uncached` is a deliberately poor default that rebuilds a root signature or a PSO on every call.
It exists as the escape hatch for the cases the cache key cannot express.

Layouts and pipelines are schemas rather than lifetime-scoped GPU memory, which is why they sit on their own pair of scopes instead of on `persistent` / `transient`.
Shader tables are `uncached`-only: one is tied to a single pipeline, so there is nothing to dedup across callers.

Beside the scopes, `ctx.routines` is the per-context render-routine registry — see [render-routines](../render-routines.md).

## The frame loop

```cpp
auto const rt = sc->acquire_backbuffer();  // also this frame's authoritative size: rt.width() / rt.height()
auto cmd = ctx->create_command_list();     // already recording
// … record into rt …
ctx->submit_command_list_and_present(*sc, std::move(cmd));
ctx->advance_epoch(sc->buffer_count());    // close the frame, throttle how far the CPU runs ahead
```

A command list is a move-only temporary, not a handle: record it on one thread, then submit or drop it exactly once, in the epoch it was opened in.
Letting one leave scope unconsumed auto-drops it and prints a warning.

`acquire_backbuffer` resizes the chain to the window if it changed, at most once per epoch, which is why the returned view — not the swapchain — is the source of truth for this frame's resolution.
`submit_command_list_and_present` is the present path, and there must be exactly one per successful acquire.
It folds the back buffer's transition to the present layout into `cmd` rather than recording a second list for it.

`advance_epoch` is the frame boundary, and it is the one context operation the caller must fence off from all others even on a `multi_threaded` backend.
Everything about what it closes, what becomes reclaimable, and how `allowed_in_flight` throttles pipelining depth is in [epochs](epochs.md).

## Lifetime and shutdown

**A context must outlive every command list and resource it created.**
That is not refcounted for you: a `raw_buffer_handle` does not keep its context alive, so holding one past the context's destruction is a use-after-free.

`shutdown()` releases all backend state and leaves the context unusable.
It is idempotent, and a backend's destructor runs it for you — call it yourself only to release the device earlier than the handle goes away.

## When things go wrong

Three tiers, and which tier applies is a property of the failure rather than of the call:

- **Contract violations `CC_ASSERT`.** A negative size, a missing usage flag, a null argument, a transient resource used past its epoch.
  These are bugs in the caller, not runtime conditions, and there is no error path for them.
- **Environment failures come back as values.** Every `create_*` has a `try_create_*` twin returning `cc::result`.
  The throwing façade is the default; the fallible core is there for exception-free callers and for local fallback — see [error-handling](../../../../../docs/error-handling.md).
- **Device loss is sticky.** A driver reset, a TDR or a removed adapter sets `is_device_lost()` once and never clears it.
  The context is then unusable and must be torn down and recreated.

Device loss has one trap worth stating plainly: **on the `try_*` surface it arrives as an ordinary, unclassified `cc::error`.**
Only the throwing façades classify it into `sg::device_lost_exception`.
So a caller that polls `try_create_*` in a retry loop must check `is_device_lost()` itself, or it will retry forever against a dead device.

## What is documented elsewhere

The context is where these mechanisms are reached, not where they are explained:

- [epochs](epochs.md) — `current_epoch` / `advance_epoch` / `process_completed_epochs` / the `wait_for_*` family, deferred deletion, finalizers.
- [threading](threading.md) — which of the above may be called concurrently, and how a build without threads makes progress.
- [async upload](upload.async.md) / [async download](download.async.md) — `ctx.upload` / `ctx.download`, the copy queues, and the automatic per-resource sync.
- [inline upload](upload.inline.md) / [inline download](download.inline.md) — the `cmd.upload` / `cmd.download` siblings that record into a list instead.
- [caches](caches.md) — `ctx.cached` vs `ctx.uncached`, the content-addressed keys, and the tiered cache behind them.
- [barriers](barriers.md) — the access tracking a backend emits barriers from, which is why recording never asks you for one.
- [presentation](presentation.md) — `create_swapchain`, `acquire_backbuffer` and `submit_command_list_and_present`, and why the acquired view carries this frame's size.

## See also

- [context.hh](../../src/shaped-graphics/context/context.hh) — the interface, including the protected `try_*` core a backend implements.
- [cheat-sheet](../../cheat-sheet.md) — the context API at a glance.
- [backends](backends.md) — what a backend is, and the duplicate-rather-than-abstract stance the one-way dependency comes from.
