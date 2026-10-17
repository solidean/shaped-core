---
created: 2026-10-17
refs:
  - file: libs/base/clean-core/src/clean-core/sequence/sequence.hh
  - file: libs/base/clean-core/docs/sequence.md
  - symbol: cc::sequence
  - issue: bug-cc-sequence-sum-and-to-array-unimplemented
---

# `cc::sequence`: the prototype's open design points

`sequence.hh` is a prototype, and its comments carried the design points it has not settled.
They are collected here; the unimplemented `sum` and `to_array` are [their own bug](bug-cc-sequence-sum-and-to-array-unimplemented.md).

- **`accumulate`'s name**, and whether its `apply` takes `(idx?, accum&, elem&)`.
- **`each` does not preserve the element's value category**: it hands `elem` on as an lvalue rather than forwarding it.
- **`to_container` / `to_vector` / `push_to` push with `push_back` alone.**
  A container protocol would make that generic and let it reserve first.
  An expiring range should move its elements rather than copy them.
- **`try_fold` and `try_fold_first` always fall back to external iteration.**
  A range that provides its own `try_fold` / `try_fold_first` should be used directly.
- **`try_fold_first` carries state between the init and the step.**
  Whether it should: if `init` and `step` return `bool` / `void`, the out parameters and pair returns go away.
