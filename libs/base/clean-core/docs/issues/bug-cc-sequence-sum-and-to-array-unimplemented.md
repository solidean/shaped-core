---
created: 2026-10-17
refs:
  - file: libs/base/clean-core/src/clean-core/sequence/sequence.hh
  - file: libs/base/clean-core/docs/sequence.md
  - symbol: cc::sequence::sum
  - symbol: cc::sequence::to_array
---

# `cc::sequence::sum()` and `to_array()` are declared but not implemented

`sum()` returns `isize` with no return statement at all, so calling it is UB — it falls off the end of a non-void function.
It needs the "special emptiness handling" the reductions section names: what an empty sequence sums to, and whether `sum` is value-returning or into-style.

`to_array()` deduces `void` from a bare `return;`, so calling it is well-defined and does nothing.
It also wants a static assert that the range has a known size.

Both compile today because nothing instantiates them.
[sequence.md](../sequence.md) is where the design for both belongs; until they exist, the declarations are traps.
