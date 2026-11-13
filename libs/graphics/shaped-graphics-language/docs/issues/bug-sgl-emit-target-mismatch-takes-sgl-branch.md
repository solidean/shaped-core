---
created: 2026-11-07
refs:
  - file: libs/graphics/shaped-graphics-language/prelude/core.sgl
---

# A module checked without a target and emitted for one takes the prelude's .sgl branch silently

What is wrong or wanted, and what is already known.

Found on 2026-11-07 while moving the double maths into the prelude (EMIT-256).
`core.sgl`'s `sqrt`, `inverse_sqrt` and `smoothstep` for doubles keep the interpreter's own answers under `const if target_language == .sgl`.
A module checked with no target and then emitted for one, as `emit_source` does, keeps that branch, so the emitted shader calls `nearest_sqrt`, which no target has.
Production compiles flatten per target, so no shipped path is known to reach it, and nothing refuses the mismatch either.

The fix is a refusal rather than a convention: emitting a module flattened for another target, `.sgl` included, should be an error naming both.
Every `const if` on the target is exposed the same way, not only these three.
