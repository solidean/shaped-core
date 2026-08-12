# shaped-graphics coding guidelines

These build on the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md) — read that first, since everything there still applies.
This document is only the **sg-specific** rules, and the places where generic advice does *not* fit sg for a reason the code does not make obvious.

**Extend it as we go.**
Catching yourself making a "style mistake" by following generic advice is the signal to add the rule here.

---

## Editorial: no meta-commentary, no contrasting with the past

Two rules on top of the repo-wide [prose style](../../../../docs/coding-guidelines.md#prose-style--one-semantic-point-per-line).

- **No meta-commentary in class/API comments about how the code is organized.**
  Where a concept "lives", that only the concept is shared while backends differ, how some other backend might realize it.
  A reader opens a header to *use* the type, so keep its comment on what the type does and which preconditions bite.
  Design context belongs in the concept doc ([docs/concepts/](_index.md)), which a header may point at in one line.

- **Never contrast with past behavior.**
  This is a greenfield build: no "before", no "previously", no "used to", no "this now fixes the old hazard".
  Such a line points at a history that never existed.
  State a mechanism's effect in the present or the conditional — "without epochs we would have a use-after-free", never "epochs fix the use-after-free we had before".

## Shared types are handed out as `*_handle` = `std::shared_ptr`

Every shared sg type `xyz` has a typedef `xyz_handle` (see [fwd.hh](../src/shaped-graphics/fwd.hh)).
Almost every one is `std::shared_ptr<sg::xyz const>` — the `const` *is* the shared-immutability below; `context_handle` and `swapchain_handle` are the mutable-driver exceptions.
Public factories return the handle, never the raw type: `create_dx12_context` returns a `context_handle`, `ctx.persistent.create_raw_buffer` a `raw_buffer_handle`.
Callers hold and pass handles, and do not construct these types by value.

**Why** (not obvious): an sg resource fronts a GPU-resident object that several in-flight command lists may reference, so shared ownership is the only model that fits.
The `_handle` suffix is the one vocabulary that makes "reference-counted GPU-side thing" legible at every call site.

**`std::shared_ptr` is the sanctioned ownership type here, and stays that way for now.**
[`cc::shared_ptr`](../../../base/clean-core/src/clean-core/memory/shared_ptr.hh) is an 8 B intrusive-refcount handle, but its Traits protocol is provisional.
See [systems/shared-ptr](../../../base/clean-core/docs/systems/shared-ptr.md).
Switching the typedefs over is what would keep sg inside the `cc` vocabulary, and [TODO](TODO.md) tracks it against that API settling.
So `std::shared_ptr` remains the one sanctioned `std::` ownership type in sg.
A new handle follows the existing pattern; do not invent a second ownership mechanism.

**`<memory>` is reached through [fwd.hh](../src/shaped-graphics/fwd.hh), never included directly.**
The smart pointers are sg's own vocabulary rather than an incidental dependency: a handle *is* a `std::shared_ptr`, and a command list *is* a `std::unique_ptr`.
So the header that declares that vocabulary is the one place that opens `<memory>`.
Both backend `fwd.hh`s and every library above sg get it from there, and `.shaped-lint.yml` scopes the blessing to that single file so a stray direct include is a finding.

## `context` / `command_list` are mutable drivers; resources are shared-immutable

- **Mutable drivers** — `context` and `command_list`, stateful and single-threaded.
  A `command_list` is recorded by one thread at a time.
- **Shared-immutable resources** — `buffer` and `texture`.
  Their **shape** (size, usage, format, …) is fixed at creation and never changes.
  They behave like a `span` over mutable GPU memory: the handle is immutable, the GPU-resident *data* it points at is not.

**Why** (not obvious): immutable shape is what makes a resource safe to share by handle across command lists with no CPU-side synchronization.
Never add a mutator that changes a resource's shape — a "resized buffer" is a *new* buffer.

## No host-visible buffers or textures; PCIe transfer is sg-managed

sg exposes **no** host-visible (CPU-mapped) buffers or textures — every `buffer` / `texture` is GPU-resident.
Host↔device transfer is a globally shared resource sg manages internally, reached through `cmd.upload` / `cmd.download` and their `ctx.upload` / `ctx.download` async twins, never by mapping memory.

**Why** (not obvious): PCIe bandwidth and staging memory are one contended global resource, and centralizing transfer is what lets sg schedule and pool staging buffers.
Host-visible memory would scatter that decision across callers and defeat the pooling.
Design a new resource API around command-list transfer, never around a CPU pointer into GPU memory.

## sg does not depend on the backends; creation lives in the backend libraries

The dependency arrow points **one way**: backends depend on sg, never the reverse.
sg (and sr, sv) must not `#include`, link, or otherwise know a concrete backend.
There is **no** `sg::create_context(backend_kind)` in the core, because the core cannot name a backend in order to create it.

Instead, **each backend library exposes a factory in the `sg` namespace** — `sg::create_dx12_context(dx12_config const&)`, `sg::create_vulkan_context(vulkan_config const&)`, ….
Each constructs its own `sg::context` subclass and returns it as a `context_handle`, and only a caller that *links that backend* sees the factory.

- The factory lives in `sg::` rather than `sg::backend::vulkan::` so every backend shares the discoverable `sg::create_*_context` prefix.
  Each takes its **own config type**, which is exactly why creation cannot live in the backend-agnostic core.
- `backend_kind` (from [types.hh](../src/shaped-graphics/types.hh)) is a **coarse, non-exhaustive tag**, not a backend identity.
  It exists to interpret raw handles handed out by escape hatches, not so the core can enumerate or switch over "the" backends.
  Treat it as an open set — a debug, cpu or remote backend is as valid as dx12/vulkan.

**Why** (not obvious): a core that never references a backend cannot overfit to today's GPUs.
A cpu reference implementation, a remote/streamed context or a capture layer drops in without touching sg.
An `sg::create_context(backend_kind)` would force the core to know every backend and invert the dependency.

## The public types are abstract; backends derive from them directly

`sg::context`, `sg::command_list` and `sg::raw_buffer` are **abstract interfaces**, and a backend subclasses them **directly** (`sg::backend::vulkan::vulkan_context : public sg::context`).
There is **no** separate bridge/impl layer mirroring the public API.

- **Cheap, shared metadata lives in the base as protected members**, with non-virtual accessors.
  A buffer's `_size_in_bytes` / `_usage` sit above the fold in `sg::raw_buffer`, so reading them costs no virtual call and every backend buffer inherits exactly them.
  Only what genuinely needs per-backend behavior is pure-virtual (`context::create_raw_buffer`).
- **Protected, not private.**
  A backend has full access to the base's state and sets it directly.
  Coupling a base to its own subclasses is intended here — this is not the Java-esque "defend my class against bad-actor subclasses" world.
  Do not wrap base state in private + getter/setter ceremony to hold subclasses at arm's length.

**Why** (not obvious): the two-layer alternative is a public `context` holding a `shared_ptr` to a parallel `backend_context`.
That duplicates every piece of shared state and makes every public method a forwarder.
One hierarchy keeps the state in one place, at the cost of a base coupled to its subclasses, which we accept.

## Backends are smurf-named and live in their own namespace

A backend type carries a redundant backend prefix ("smurf naming") **and** lives in a per-backend namespace: `sg::backend::dx12::dx12_context`, `sg::backend::vulkan::vulkan_command_list`.

**Why** (not obvious): the `dx12_` / `vulkan_` prefix makes every symbol of one backend greppable in a single search, and the namespace keeps same-role types from clashing.
The usual "don't stutter the namespace in the type name" advice does not apply — here the stutter is the point.

## Duplicate across backends rather than abstract

We deliberately **share very little code between backends**, and treat the resulting duplication as fine.
Where dx12 and vulkan need similar-looking logic, write it twice rather than hoist a shared cross-backend helper.

**Why** (not obvious): a cross-backend abstraction layer leaks — it grows a conditional per API quirk, and each backend becomes harder to read than its own straight-line version.
Independent per-backend code stays readable as the APIs diverge.
Shared code belongs *below* sg (the sg core, clean-core, typed-geometry), never in a cross-backend layer inside `backends/`.

## Backend code is public and optimized for readability, not encapsulation

A backend library is held to a *different* bar than the sg core: largely public, with little encapsulation.
Small methods live inline in the header, types expose their guts, and readable beats hidden — `kind()` and the `sg::create_*_context` factory are defined in the header, not tucked into a `.cc`.

**Why** (not obvious): from sg's and sr's side the backend is *already* opaque, since they cannot depend on it, so a second layer of encapsulation inside it buys nothing.
The audience for backend internals is someone debugging that backend, and they are best served by code they can read top to bottom.

## Reaching the underlying backend type is a "here be dragons" escape hatch

The concrete backend types are public and the `sg::context` handle *is* that object, so you can recover the backend type: `dynamic_cast<sg::backend::vulkan::vulkan_context*>(ctx.get())`.
That is a deliberate, unpoliced escape hatch, and fully "you are on your own" — the code is now coupled to one backend and its version.
Reach for it only for backend-specific behavior sg does not expose.

A **blessed** middle ground is planned: an API handing back the raw underlying GPU handles without exposing the concrete backend *types* — tracked in [TODO](TODO.md).
Prefer it once it exists.

## Backends expose backend-typed create methods; the virtuals are thin forwarders

Each abstract `sg::context` method — `create_command_list`, `create_raw_buffer`, `submit_command_list`, … — is a **one-line forwarder** to a **non-virtual, backend-typed** method.
`dx12_context::create_dx12_buffer` returns a `dx12_buffer_handle`; `create_dx12_command_list` returns `std::unique_ptr<dx12_command_list>`.
The `override` calls the backend-typed method and up-casts the result.

- **Prefer the backend-typed method** whenever you already hold the concrete `dx12_context` — inside the backend, or in a backend test.
  You get the concrete type back with no downcast.
- **The heavy body lives once**, in the backend-typed method in the `.cc`; the forwarder stays trivial and inline in the header.

**Why** (not obvious): backends are never mixed — a `dx12_context` only ever deals in `dx12_*` objects.
Routing through the concrete types therefore removes the swarm of `static_cast`s a "virtual does everything on the base types" design would force.
Each backend stays readable in its own vocabulary, and the virtual layer exists only for a caller that genuinely holds the abstract `sg::context`.

## Command lists are held by `unique_ptr`, never a handle

A resource is shared, per the `*_handle` rule above, plus a **backend-typed** handle — `dx12_buffer_handle` — for backend code.
A **command list is a move-only temporary**: record once, submit once, never reused.
It is held by `std::unique_ptr<command_list>` (polymorphic, so not `cc::unique_ptr`), and there is **deliberately no `command_list_handle` typedef** and no backend-typed command-list handle.
The `unique_ptr` lives in a handful of places; everything else takes the list by reference (`command_list&`).

**Why** (not obvious): a command list is a throwaway recording owned by one place, so a unique, move-only owner is both cheaper and more honest than a shared handle.
Not minting a typedef for the rare `unique_ptr` keeps that ownership visible at the few sites that hold it.

## Context outlives its objects; explicit `drop`/`shutdown` unwind bookkeeping

- **Global lifetime invariant:** a `context` must outlive **every** command list and resource it created.
  Backend objects hold a **literal backref** (`dx12_context&`, not a `weak_ptr`) to their creating context, and use it on teardown.
- **`submit` / `drop` consume the command list.**
  Both take it by value as a `std::unique_ptr<command_list>`, so you move it in.
  That makes *submit once* and *drop once* structural — the list is gone afterward, with no flag to track.
  `ctx.drop_command_list(std::move(cmd))` is exactly "let it go out of scope now": both paths destroy the list, and the **destructor** is the single teardown point.
- **`context::shutdown()` is virtual and idempotent.**
  A context **must be shut down before destruction** — the base `~context` asserts it, and each backend destructor calls `shutdown()`, so the ordinary destruction path already satisfies the invariant.
  Each backend **overrides** `shutdown()` to release its own resources, and duplicating the idempotency guard across backends is fine (there is no separate `on_shutdown` hook).

**Why** (not obvious): moving the list into `submit` / `drop` lets the type system enforce single use instead of a runtime flag, and collapses "explicit drop" and "scope exit" onto one code path.
The backref plus the outlives rule is what makes that teardown safe to run without a `weak_ptr` check on every operation.

## Default to the typed wrappers: `buffer<T>` / `texture_2d`, not `raw_*`

Reach for the typed factory first, in sg code and in callers/tests alike:

```cpp
auto const vbuf = ctx.persistent.create_buffer<tg::pos3f>(cube.size(), usage);   // prefer
auto const img = ctx.transient.create_texture_2d({.format = …, .width = …});     // prefer

auto const vbuf = ctx.persistent.create_raw_buffer(count * sizeof(tg::pos3f), usage);  // avoid
```

The typed wrapper counts in elements, not bytes.
Its view factories (`as_uniform_buffer()`, `as_readwrite_view()`, …) infer the element type and are `requires`-gated.
A nonsensical binding is then a compile error rather than a driver complaint.

**The transfer API takes typed buffers directly** — `cmd.upload.data_to_buffer(buf, range)`,
`pod_to_buffer(buf, value)`, `cmd.download.data_from_buffer(buf)`, and the `ctx.upload`/`ctx.download`
async twins.
Range element, pod value and `buffer<T>` all agree on the same `T`, so a mismatch is a compile error
and `T` never has to be spelled out.
**A `.raw()` in a transfer call is a smell** — it means an overload is missing; add it rather than
unwrapping at the call site.

`raw_*` stays the escape hatch for byte-addressed work, and for a struct field that genuinely holds a
`raw_buffer_handle` — a `blas_triangles`'s vertex buffer, say.
Those reach through `.raw()`.

## Per-frame resources are `ctx.transient`, not `ctx.persistent`

Anything sized by the current frame is created from `ctx.transient` and expires with the epoch — an offscreen render/trace target, a scratch buffer, a binding group.
Don't hoist it into a persistent handle plus a hand-rolled "did the size change?" cache.
The transient scope already does that, more cheaply, and the cache is one more thing to get wrong on a resize.

`ctx.persistent` is for what genuinely outlives a frame.
Meshes, acceleration structures, and buffers whose *contents* are refreshed per frame but whose identity is stable.

## Resources may be empty (size 0)

A `buffer` of size 0 is **valid** — an empty buffer, like an empty `span` / `vector`.
It allocates **no** GPU storage; the dx12 backend keeps a null underlying resource, since D3D12 cannot create a zero-width committed resource.
Only a **negative** size is programmer misuse, and asserts.
Do not add a "non-empty" precondition to resource creation — empty is a normal, representable state.
