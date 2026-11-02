---
created: 2026-10-17
tags: [vulkan, metal]
refs:
  - file: libs/graphics/shaped-graphics/docs/concepts/barriers.md
  - file: libs/graphics/shaped-graphics/tests/barrier/footprint-barrier-test.cc
  - file: libs/graphics/shaped-graphics/tests/compute/compute-texture-test.cc
  - symbol: resource_access_state::flush
  - symbol: render_pass_split_warnings
---

# Declaring a scope's accesses up front, so their barriers land outside it

Declare a scope's accesses before the rendering scope opens or at its start, so their barriers are emitted outside it.

A backend cannot always emit a barrier inside a rendering scope: vulkan never can, and metal cannot for a fragment-stage source.
Each one found only at its draw suspends the scope, stores and reloads every target, and warns once per context (`ctx.metrics.render_pass_split_warnings`).
A resource a draw samples for the first time in the scope is the usual case: its transition is known before the scope.

**`cmd.ensure_layout` is not that declaration, and must not be used as one.**
It declares no access, so its barrier has an empty destination and the tracker drops the write before it.
An access later in the same list then goes unordered against that write, which is a race on dx12 today (`resource_access_state::flush`).
It is safe only for a consumer after the submit, which is every use it has.
[bug-sg-pure-layout-transition-empty-scope](bug-sg-pure-layout-transition-empty-scope.md) is the same empty scope seen from the transfer side.

Not every transition is even legal inside a scope, so the declaration has to reach the barrier before the scope opens, not just avoid the split.

Tests and examples that touch a resource once before a scope, to keep its barrier out of the scope, carry an `issue:` marker naming this.
