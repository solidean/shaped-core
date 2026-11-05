---
created: 2026-11-05
parent: epic-sgl-float-guarantee-ladder
refs:
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/float-guarantees.md
---

# Warn on a NaN or infinity test in code compiled at fast

`isnan(x)`, `isinf(x)` and `x != x` in a function whose entry point is at `fast` test what the compiler may assume never happens, and may be deleted.
On dx12 without `-Gis` the float-specials probe's NaN comparisons all came out wrong for exactly that reason.
A warning there, naming `@float(.specials)` as the fix, catches it where it is written.
