---
created: 2026-10-17
parent: epic-sgl-prelude-check-cost
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check.cc
  - symbol: sgl::checked_prelude
  - test: sgl profile - checking a small program behind the prelude, sampled
---

# A compile still copies the checked prelude

A check behind `checked_prelude()` starts from a copy of the whole checker state, the prelude's side tables included.
With the whole-module passes that run over the prelude's part again, that is about 1.9 ms of the 2.9 ms a ten-line compute shader takes on `debug-linux-clang`.
Sharing the prelude's part read-only, and running those passes over the program's part alone, would leave the program's own check.
The test `sgl profile - checking a small program behind the prelude, sampled` measures both.
