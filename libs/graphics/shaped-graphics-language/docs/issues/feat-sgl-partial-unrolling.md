---
created: 2026-10-17
refs:
  - file: libs/graphics/shaped-graphics-language/tests/corpus/control/unroll.sgl
---

# Partial unrolling, `@unroll(n)`

`@unroll(n)`, writing a loop's body `n` times per iteration, is `unsupported-yet` (CHK-389).
It is the form for a range too long to unroll whole, or one whose bound is no constant.
