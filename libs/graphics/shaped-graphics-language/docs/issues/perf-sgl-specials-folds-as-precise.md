---
created: 2026-11-25
refs:
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/ir/simplify.cc
  - file: libs/graphics/shaped-graphics-language/docs/spec/incubator/float-guarantees.md
---

# SGL's IR folds what .specials allows only at .fast

The float-guarantee ladder lets `.specials` reassociate, contract and divide through a reciprocal, keeping only NaNs, infinities and signed zeros.
SGL's own SPIR-V and DXIL now state no more than that at `.specials`, and `chain_sums` reorders a sum there as at `.fast`.
Three folds in `ir/simplify.cc` still take `.fast` alone, so a `.specials` entry point keeps work the ladder lets it drop:

- `reassociate`: `(v * c) / d` as `v * (c / d)`, which is reassociation alone; it is reached only through `fold_fast`, whose other folds do need `.fast`.
- `fold_lanes`'s `dot` of constant lanes, summed in one order, which reassociation allows.
- `fold_lanes`'s `math`, `fold_math`: which of its folds are exact, reassociating or `.fast`'s own is not sorted out.

`fold_fast`'s folds by a known zero or one (`x * 0`, `x + 0`, `0 / x`) assume no NaN and ignore a zero's sign, so they stay `.fast`'s.
Each fold wants splitting by the license it uses, with a test at `.specials` that it folds and at `.precise` that it does not.
Nothing is measured: how often a `.specials` shader meets these folds is not known.
