---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics/src/shaped-graphics/fwd.hh
  - file: libs/base/clean-core/src/clean-core/memory/shared_ptr.hh
---

# The `*_handle` typedefs still use `std::shared_ptr`

[`cc::shared_ptr`](../../../../base/clean-core/src/clean-core/memory/shared_ptr.hh) exists — 8 B, intrusive, Traits-keyed.
But its Traits protocol is provisional, shaped by `cc::async`'s needs and expected to be simplified.
So this is gated on that API settling rather than ready to pick up: see [systems/shared-ptr](../../../../base/clean-core/docs/systems/shared-ptr.md).

It will not be a drop-in even then.
sg's resources are polymorphic, so `default_shared_traits`' `sizeof(T)`-derived control offset cannot find the counts through a base-typed handle — the same blocker slib hits.
They also derive from `std::enable_shared_from_this`, with 30+ `shared_from_this()` call sites and no `cc::shared_ptr` equivalent.
See the [coding-guidelines](../coding-guidelines.md) note.
