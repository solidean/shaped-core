# shaped-graphics coding guidelines

These build on the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md) — read
that first; everything there still applies. This document only captures the **sg-specific**
rules and the places where generic advice does *not* apply to sg for non-obvious reasons.

It is intentionally thin for now. **Extend it as we go:** whenever you catch yourself making a
"style mistake" by following generic advice that turns out to be wrong for sg (for a reason
that isn't obvious from the code), that's the signal to add the rule here.

---

## Shared types are handed out as `*_handle` = `std::shared_ptr`

Every shared sg type `xyz` has a typedef `xyz_handle = std::shared_ptr<sg::xyz>` (see
[fwd.hh](../src/shaped-graphics/fwd.hh)). Public factories return the handle, not the raw type:
`create_context` returns `context_handle`, `create_buffer` returns `buffer_handle`. Callers
hold and pass handles; they don't construct these types by value.

**Why** (not obvious): sg resources front GPU-resident objects with non-trivial lifetime and
sharing (a buffer may be referenced by several in-flight command lists). Shared ownership is
the natural model, and a single vocabulary — the `_handle` suffix — makes "this is a
reference-counted GPU-side thing" legible at every call site.

**`std::shared_ptr` is a deliberate placeholder.** clean-core has `cc::unique_ptr` but **no**
`cc::shared_ptr` yet. Per the repo's "surface missing pieces" rule, the clean solution is a
`cc::shared_ptr` in clean-core that these handles would switch to — it keeps sg inside the `cc`
vocabulary and off `std::`. Until that lands, `std::shared_ptr` is used and this is the one
sanctioned `std::` ownership type in sg. If you add a new handle, follow the existing pattern;
don't invent a second ownership mechanism.

## `context` / `command_list` are mutable drivers; resources are shared-immutable

Two categories of type, treated differently:

- **Mutable drivers** — `context` and `command_list`. These are stateful and single-threaded:
  a `command_list` is recorded by one thread at a time. They own their backend bridge object.
- **Shared-immutable resources** — `buffer` (and, coming, `texture`). Their **shape** (size,
  usage, format, …) is fixed at creation and never changes; they behave like a `span` over
  mutable GPU memory — the handle is immutable, the GPU-resident *data* it points at is not.

**Why** (not obvious): treating resources as immutable-shape values is what makes them safe to
share by handle across command lists without synchronization on the CPU side. Don't add mutators
that change a resource's shape; a "resized buffer" is a *new* buffer.

## No host-visible buffers or textures; PCIe transfer is sg-managed

sg exposes **no** host-visible (CPU-mapped) buffers or textures. All `buffer`/`texture` objects
are GPU-resident. Host↔device transfer is a globally shared resource that sg manages internally
(via command-list upload/download); it is not something a caller reaches by mapping memory.

**Why** (not obvious): PCIe bandwidth and staging memory are a single contended, global resource.
Centralizing transfer in sg lets it schedule and pool staging buffers; exposing host-visible
memory would scatter that decision across callers and defeat the pooling. Design new resource
APIs around command-list transfer, never around a CPU pointer into GPU memory.

## sg does not depend on the backends; creation lives in the backend libraries

The dependency arrow points **one way**: backends depend on sg, never the reverse. sg (and sr,
sv) must not `#include`, link, or otherwise know any concrete backend. There is **no**
`sg::create_context(backend_kind)` in the core — the core cannot name a backend to create it.

Instead, **each backend library exposes a factory in the `sg` namespace**:
`sg::create_dx12_context(dx12_config const&)`, `sg::create_vulkan_context(vulkan_config const&)`,
… Each constructs its own `sg::context` subclass (e.g. `vulkan_context`) and returns it as a
`context_handle`. Only a caller that *links that backend* sees the factory.

- The factory lives in `sg::` (not `sg::backend::vulkan::`) so all backends share the
  discoverable `sg::create_*_context` prefix — but each takes its **own config type**, which is
  exactly why creation can't live in the backend-agnostic core.
- `backend_kind` (from [types.hh](../src/shaped-graphics/types.hh)) is a **coarse, non-exhaustive
  tag**, not a backend identity. It exists to interpret raw handles from escape hatches, not so
  the core can enumerate or switch over "the" backends. Don't treat it as a closed set — a debug,
  cpu, or remote backend is just as valid as dx12/vulkan.

**Why** (not obvious): this decoupling keeps sg from overfitting to today's GPU backends. Because
the core never references a backend, entirely different "backends" (a cpu reference
implementation, a remote/streamed context, a capture/debug layer) drop in without touching sg.
An `sg::create_context(backend_kind)` in the core would force the core to know every backend and
invert the dependency — precisely what we avoid.

## The public types are abstract; backends derive from them directly

`sg::context`, `sg::command_list`, and `sg::buffer` are **abstract interfaces**, and a backend
subclasses them **directly** (`sg::backend::vulkan::vulkan_context : public sg::context`). There
is **no** separate bridge/impl layer mirroring the public API — a public type holding a
`shared_ptr` to a parallel `backend_*` interface would duplicate the state (once in the public
type, once in the impl) and turn every public method into a thin forwarder. One hierarchy avoids
both.

- **Cheap, shared metadata lives in the base as protected members**, with non-virtual accessors —
  a buffer's `_size_in_bytes` / `_usage` sit "above the fold" in `sg::buffer`, so reading them
  costs no virtual call and every backend buffer inherits exactly them. Operations that genuinely
  need per-backend behavior are the pure-virtual methods (e.g. `context::create_buffer`).
- **Protected, not private.** Backends have full access to the base's state and set it directly.
  The coupling between a base and its own subclasses is fine and intended — this is **not** the
  Java-esque "defend my class against bad-actor subclasses" world. Don't wrap base state in
  private + getter/setter ceremony to hold subclasses at arm's length.

**Why** (not obvious): a two-layer design (a public `context` holding a `shared_ptr` to a
`backend_context`, etc.) duplicates all shared state and makes every public method a forwarder. A
single hierarchy — abstract base + protected state + backend subclass — keeps the state in one
place and the code direct, at the cost of coupling a base to its subclasses, which we accept.

## Backends are smurf-named and live in their own namespace

Backend types carry a redundant backend prefix ("smurf naming") **and** live in a per-backend
namespace: `sg::backend::dx12::dx12_context`, `sg::backend::vulkan::vulkan_command_list`. Both
the prefix and the namespace are intentional.

**Why** (not obvious): backend code is greppable and non-colliding by construction. `dx12_` /
`vulkan_` prefixes make it trivial to search all of one backend's symbols; the namespace keeps
same-role types from different backends (`dx12_context` vs `vulkan_context`) from clashing.
This is contrary to the usual "don't stutter the namespace in the type name" advice — here the
stutter is a feature.

## Duplicate across backends rather than abstract

We deliberately **share very little code between backends** and treat the resulting duplication
as fine. When dx12 and vulkan need similar-looking logic, prefer writing it twice over hoisting
a shared cross-backend helper/abstraction.

**Why** (not obvious): in past incarnations, cross-backend abstraction layers made each backend
harder to read and maintain (leaky abstractions, conditionals for API quirks). Independent
per-backend code — even with duplication — has proven easier to understand and evolve. Shared
code belongs *below* sg (in the sg core, clean-core, or typed-geometry), not in a cross-backend
layer inside `backends/`.

## Backend code is public and optimized for readability, not encapsulation

Backend libraries are held to a *different* bar than the sg core. They are largely public and
have little encapsulation: small methods live inline in the header, types expose their guts, and
we favor obvious, readable code over information hiding. `kind()` and the `sg::create_*_context`
factory are defined right in the header, not tucked into a `.cc`.

**Why** (not obvious): from sg's and sr's perspective the backend is *already* opaque — they
cannot depend on it — so a second layer of encapsulation inside the backend buys little and costs
readability. The audience for backend internals is someone debugging that specific backend, and
they are best served by code they can read top-to-bottom.

## Reaching the underlying backend type is a "here be dragons" escape hatch

Because the concrete backend types (`sg::backend::vulkan::vulkan_context`, …) are public and the
`sg::context` handle *is* that object, you *can* recover the backend type — e.g.
`dynamic_cast<sg::backend::vulkan::vulkan_context*>(ctx.get())`. That is a deliberate, unpoliced
escape hatch: it works, and it is fully "you are on your own" territory (you have coupled your code
to a specific backend and its version). Use it only when you truly need backend-specific behavior
sg doesn't expose.

A **blessed** middle-ground escape hatch is planned for sg: an API that hands back the raw
underlying GPU handles (native device/buffer/… handles) **without** exposing the concrete backend
*types*. Prefer that once it exists; reserve the `dynamic_cast`-to-backend-type route for the rare
cases the blessed hatch doesn't cover.
