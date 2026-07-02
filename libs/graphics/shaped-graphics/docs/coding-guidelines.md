# shaped-graphics coding guidelines

These build on the repo-wide [coding-guidelines](../../../../docs/coding-guidelines.md) — read
that first; everything there still applies. This document only captures the **sg-specific**
rules and the places where generic advice does *not* apply to sg for non-obvious reasons.

It is intentionally thin for now. **Extend it as we go:** whenever you catch yourself making a
"style mistake" by following generic advice that turns out to be wrong for sg (for a reason
that isn't obvious from the code), that's the signal to add the rule here.

---

## Editorial: no meta-commentary, no contrasting with the past

Two rules for how we write sg comments and docs, on top of the repo-wide concise-comment guidance.

- **No meta-commentary in class/API comments about how the code is organized** — where a concept
  "lives", that only the concept is shared while backends differ, how some other backend might
  realize it, and so on. A user reads a header to *use* the type; keep its comment about what it does
  and the preconditions that matter. That kind of design context belongs in the concept doc
  ([docs/concepts/](_index.md)), which a header may point to in one line.

- **Never contrast with past behavior.** This is a greenfield build — there is no "before", no
  "previously", no "used to", no "this now fixes / removes the old hazard". Such lines reference a
  history that never existed and are pure noise. Describe what *is*. If a mechanism prevents a
  problem, state it in the present or conditional: write "without epochs we would have a
  use-after-free", **not** "epochs fix the use-after-free we had before".

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

## Backends expose backend-typed create methods; the virtuals are thin forwarders

The abstract `sg::context` methods (`create_command_list`, `create_buffer`, `submit_command_list`,
…) are implemented in a backend as **one-line forwarders** to **non-virtual, backend-typed**
methods: `dx12_context::create_dx12_buffer` returns a `dx12_buffer_handle`;
`create_dx12_command_list` returns `std::unique_ptr<dx12_command_list>`. The `override` simply calls
the backend-typed method and up-casts the result (`dx12_buffer_handle → buffer_handle`,
`unique_ptr<dx12_command_list> → unique_ptr<command_list>`).

- **Prefer the backend-typed method** whenever you already hold the concrete `dx12_context` (e.g.
  inside the backend or in a backend test). You get the concrete type back with **no downcast**.
- **The heavy body lives once**, in the backend-typed method (in the `.cc`); the forwarder is
  trivial and stays inline in the header.

**Why** (not obvious): the prototype found this the favored pattern *everywhere*. Backends must
never be mixed — a `dx12_context` only ever deals in `dx12_*` objects — so routing through the
concrete types removes a swarm of `static_cast`s that a "virtual does everything on the base types"
design forces, and keeps each backend readable in its own vocabulary. The virtual layer exists only
for callers who genuinely hold the abstract `sg::context`.

## Handles: shared for resources, `unique_ptr` for command lists, references to operate

- **Resources are shared** — `buffer` (and coming `texture`) use `buffer_handle`
  (`std::shared_ptr`), plus a **backend-typed** handle (`dx12_buffer_handle`) for backend code.
- **Command lists are move-only temporaries** — `record once, submit once, not reused`. They are
  held by **`std::unique_ptr<command_list>`** (polymorphic, so not `cc::unique_ptr`), and there is
  **deliberately no `command_list_handle` typedef** and no backend-typed command-list handle. The
  `unique_ptr` lives in a handful of places; everything else takes the list **by reference**
  (`command_list&`), or uses `auto`.

**Why** (not obvious): sharing is the right model for GPU resources referenced by several in-flight
command lists; a command list is a throwaway recording owned by one place, so a unique, move-only
owner is both cheaper and more honest than a shared handle. Not minting a typedef for the rare
`unique_ptr` keeps the ownership visible at the few sites that hold it.

## Context outlives its objects; explicit `drop`/`shutdown` unwind bookkeeping

- **Global lifetime invariant:** a `context` must outlive **every** command list and resource it
  created. Backend objects hold a **literal backref** (`dx12_context&`, not a `weak_ptr`) to their
  creating context and use it on teardown. If violating this ever becomes a real problem we can
  revisit `weak_ptr`; the prototype never hit it.
- **`submit`/`drop` consume the command list.** Both take it **by value as a
  `std::unique_ptr<command_list>`** — you move it in. That makes *submit once* and *drop once*
  structural (the list is gone afterward, no flags to track). An explicit
  `ctx.drop_command_list(std::move(cmd))` is exactly "let it go out of scope now": both destroy the
  list, and the **destructor** is the single teardown point where future resource-tracking unwinds.
- **`context::shutdown()` is virtual and idempotent.** A context **must be shut down before
  destruction**; the base `~context` asserts it, and each backend destructor calls `shutdown()` so
  the ordinary destruction path satisfies the invariant. Each backend **overrides** `shutdown()` to
  release its own resources — **duplicating the idempotency guard across backends is fine** (there is
  no separate `on_shutdown` hook).

**Why** (not obvious): moving the list into `submit`/`drop` lets the type system enforce
single-use instead of runtime flags, and collapses "explicit drop" and "scope exit" onto one code
path (destruction). The backref + "context outlives its objects" rule is what makes the destructor's
teardown safe to run without a `weak_ptr` check on every operation.

## Resources may be empty (size 0)

A `buffer` of size 0 is **valid** — an empty buffer, like an empty `span`/`vector`. It allocates
**no** GPU storage (dx12 can't create a zero-width committed resource anyway; the backend keeps a
null underlying resource). Only a **negative** size is programmer misuse and asserts. Don't add a
"non-empty" precondition to resource creation; empty is a normal, representable state.
