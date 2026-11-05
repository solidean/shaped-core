---
created: 2026-11-05
refs:
  - file: libs/base/clean-core/src/clean-core/memory/unique_ptr.hh
---

# poly_unique_ptr: an owning pointer that upcasts to a polymorphic base

`cc::unique_ptr<T>` owns exactly one T and deliberately has no upcast: it frees through `cc::node_allocation<T>`, which knows T's size and alignment, not the derived type's.
So a non-template struct cannot own an object whose type is a template parameter of the functions that use it.
Example: a container of records shared by functions templated over an arithmetic, each record holding data whose layout depends on that arithmetic.
Today a consumer serializes such data into bytes, or reaches for `std::unique_ptr`, which the lint discourages.
`cc::shared_ptr` upcasts only within one Traits, its default control block sized by the static type.
`cc::poly_node_allocation` can own through a base, but needs every base to report its node class through a virtual: too low-level for an interface others implement.

Wanted: `cc::poly_unique_ptr<Base>`, move-only, made by `cc::make_poly_unique<Derived>(...)` and converting from `poly_unique_ptr<Derived>`.
- It frees what it was made with: the size, alignment and destructor of the most derived type, captured at make, not taken from `Base`.
  Either a virtual destructor on `Base` with the allocation's size beside the pointer, or a per-type destroy function pointer, which also serves a `Base` without a vtable.
- The same node memory resource as `unique_ptr`, so freeing stays wait-free.
- `get`, `*`, `->`, `is_valid`, assignment from nullptr; a `static_cast`-like downcast for a caller who knows the type.

Open to decide: one pointer wide with the destroy function in a header before the object, or two pointers wide; whether `unique_ptr` itself should grow the upcast instead of a second type.
