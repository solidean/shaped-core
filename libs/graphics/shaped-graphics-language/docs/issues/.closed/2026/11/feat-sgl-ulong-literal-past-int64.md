---
created: 2026-11-05
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/semantics/checking.md
closed: 2026-11-06
resolution: fixed
---

# A ulong literal past 2^63 - 1

An integer literal is held in an `i64`, so a `ulong` literal past 2^63 - 1 is refused; `~0u64` and a shift are the workaround.
The literal's value wants a `u64` beside its sign, through the parser, the check pass's constants and every emitter.
