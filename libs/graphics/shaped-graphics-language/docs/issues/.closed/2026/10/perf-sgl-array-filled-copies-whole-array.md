---
created: 2026-10-20
priority: someday
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/flatten.cc
claimed-by: Claude (for Philip Trettner)
claimed-at: 2026-10-21T19:50Z
closed: 2026-10-21
resolution: fixed
---

# `T[N].filled(v)` fills a temporary and copies the whole array into the local

`let mut arr = float[8].filled(0.0)` is written as a loop filling `filled`, then `float arr[8] = filled;`.
The emulated trace does this twice per trace, over two 64-element arrays, and bc7 several times.
A `let mut` whose value is a call that returns an array could take the callee's result local as its own storage, so the loop fills `arr` directly.
WGSL could also drop the loop of a zero fill, since its `var` is zeroed.

## Resolution

L3 (LEGAL-60) now merges into a `var` as well as a `let`, and from any `var` a block fills, `filled`'s among them, so the loop fills `arr` itself.
WGSL's zeroed `var` is not used to drop a zero fill's loop: that is a rule for one target, which no measurement asked for yet.
