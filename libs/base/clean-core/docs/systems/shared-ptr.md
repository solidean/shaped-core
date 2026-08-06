# cc::shared_ptr

`cc::shared_ptr<T, Traits>` / `cc::weak_ptr<T, Traits>` ([shared_ptr.hh](../../src/clean-core/memory/shared_ptr.hh)) are an 8 B, single-pointer, intrusive-refcount handle pair.

**The design is provisional.**
It exists because [`cc::async`](async.md) needs a handle that is one pointer wide over a node it already owns.
async is still its only user — every other shared handle in the repo is a `std::shared_ptr`.
The Traits protocol below is what that requirement produced, not a settled extension point.
Expect it to be simplified, and do not add a third stock Traits without expecting to rewrite it.
What will not move is the lifetime contract: strong/weak counting and the `release`/`adopt` rules are correctness, not API shape.

## The model

The handle stores exactly the payload pointer, so `get()` is a no-op and there is no second pointer to a control block.
The counts live in the same node, located by the `Traits` type, which is what buys the 8 B.

Two layouts exist today, and they are the two cases that were needed rather than a taxonomy:

- **trailing control** (`cc::default_shared_traits<T>`) — the node is `[ T payload | control ]`, with the counts at a fixed offset past `T`, so nothing intrudes on `T` itself.
  `shared_ptr<int>` is a 16 B node.
- **intrusive** (`cc::impl::async_node_traits`) — the counts are members of `T`, so the node *is* the object.
  Keyed on a base class, so every `async<U>` shares one Traits through the implicit upcast.

`make_shared` allocates one node from the node allocator — a slab slot for the small classes, and the header-backed large path above 256 B. See [node-allocation](node-allocation.md).

## Lifetime

Standard shared/weak counting, with one rule that the intrusive layout makes sharp:
**the strong owners collectively hold one weak count.**
`destroy_object` fires when strong reaches 0, `free_storage` when weak then reaches 0.

The counts must stay readable between those two points, so `destroy_object` must not destroy them.
With trailing control that is automatic, since the control is separate storage; an intrusive Traits must run a payload-only teardown instead, which is what async's node does.
`Traits::release_strong` reports what the caller must do next, and the order it reports is load-bearing.
[shared_ptr.hh](../../src/clean-core/memory/shared_ptr.hh)'s header block carries the case table and the reason, and is the single authority on it.

## The two escape hatches, and why they differ

- `from_alive(p)` **mints** a new count.
  The storage must be known alive (strong or weak > 0); undefined once it has been freed.
- `release()` / `adopt()` **move** an existing count and mint nothing — count-neutral by construction.

`release`/`adopt` exist so a strong count can be parked in hand-rolled storage without a redundant inc/dec round trip, and whoever holds the raw pointer then owes the release.
Neither hatch adopts an arbitrary raw pointer.

## Constraints

`cc::make_shared<T, Traits>(...)` is the only way to construct one, and a node is born owned at strong = 1.
Upcasts to a base with the **same** Traits are allowed; aliasing or projecting to a subobject is deferred rather than planned.
`weak_ptr` needs a Traits with `supports_weak`, and a strong-only Traits static-asserts.

## Why the rest of the repo is still on std::shared_ptr

Not an oversight, and worth knowing before "migrating" anything:

- shaped-graphics' resources are polymorphic — `dx12_buffer final : public sg::raw_buffer`.
  So `default_shared_traits`' `sizeof(T)`-derived control offset cannot find the counts through a base-typed handle.
  They also derive from `std::enable_shared_from_this`, with 30+ `shared_from_this()` call sites that `cc::shared_ptr` has no equivalent for.
  See [sg's TODO](../../../../graphics/shaped-graphics/docs/TODO.md).
- shaped-shader-library's filesystem handles are polymorphic, and `default_shared_traits` places the control block at an offset derived from `sizeof(T)`.
  So a base-typed handle to a derived object would look for the counts in the wrong place.
  That needs a base-keyed Traits, which is the same shape async had to write by hand.

For the API surface see the [cheat sheet](../../cheat-sheet.md).
