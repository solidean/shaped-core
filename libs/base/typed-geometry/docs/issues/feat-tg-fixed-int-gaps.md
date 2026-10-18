---
created: 2026-10-17
refs:
  - file: libs/base/typed-geometry/src/typed-geometry/scalar/fixed_int
  - symbol: tg::fixed_int
---

# `fixed_int`: what the first cut leaves out

- Mixed signed / unsigned heterogeneous operands.
- Generated loop-free widening shifts.
- Generated bit counts and float conversions, both short loops today.
- The bounded quotient's remainder at the divisor's width, where the divisor's bound allows it.
