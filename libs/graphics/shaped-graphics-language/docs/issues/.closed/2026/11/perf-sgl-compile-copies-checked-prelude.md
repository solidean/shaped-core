---
created: 2026-10-17
parent: epic-sgl-prelude-check-cost
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/check/check.cc
  - symbol: sgl::checked_prelude
  - test: sgl profile - checking a small program behind the prelude, sampled
closed: 2026-11-01
resolution: fixed
---

# A compile still copies the checked prelude

A check behind `checked_prelude()` starts from a copy of the whole checker state, the prelude's side tables included.
With the whole-module passes that run over the prelude's part again, that is about 1.9 ms of the 2.9 ms a ten-line compute shader takes on `debug-linux-clang`.
Sharing the prelude's part read-only, and running those passes over the program's part alone, would leave the program's own check.
The test `sgl profile - checking a small program behind the prelude, sampled` measures both.

## Resolved

The compilation model's step 1 shares the checked prelude instead of copying it.
Its tables are `layered`, and the scopes and caches a program extends are `layered_map`s.
The whole-module passes that walked the prelude's part again start behind it.
`sgl profile - checking a small program behind the prelude, sampled`, release build, Windows: 0.61 ms before, 0.10 ms after; an empty program 0.27 ms before, 0.05 ms after.
What a compile still copies is the checker's smaller append-only lists (`calls`, `notes`, `function_arguments`), which no longer show in the profile.
