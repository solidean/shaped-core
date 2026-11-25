---
created: 2026-11-25
refs:
  - issue: feat-sgl-interpreter-bounds-float-departures
  - file: libs/graphics/shaped-graphics-language/src/shaped-graphics-language/interpret/impl/bounds.cc
---

# A bounding run widens a sum by what reassociating it may cost

`.fast` and `.specials` let a target reassociate (EVAL-6), and a bounding run models no reassociation yet: the file's tolerance covers it, which is the same mismatch `half_arithmetic.sgl` had.
Reassociation is no widening of one operation, since it changes which operations there are, so the per-call rule in `bound_call` cannot express it.

## The shape

- **Find each sum chain statically**, once per entry point: a tree of `+` and `-` of one float type whose inner nodes have no other use.
  A GPU compiler reassociates through SSA, so a chain runs through a `let` read once, and stops at a value read twice.
- **Evaluate the chain's terms**, each with its own range, then bound every order at once.
  Any order of `n` terms rounds to within `γ(n-1) · Σ|x_i|` of the exact sum, `γ(k) = k·u / (1 - k·u)` and `u` the unit roundoff, 2^-24 for a float and 2^-11 for a half kept as one.
  That is Higham's bound, and it holds whatever the order and whatever is fused into it.
- **The chain's result is the hull** of the exact sum over the terms' ranges, widened by that bound.
- A product chain is the same with a relative bound, `(1 + u)^(n-1)`.

## Done

- **Chains are found at run time, not statically**: a `+` or `-` merges its operands' chains, `scalar::terms` and its neighbors.
  That follows a chain through any `let`, and it stays sound for a value read twice, since a compiler may reassociate the copy anyway.
- **A `dot` or a matrix product is a chain of products** rounded step by step at every level, by γ(n) of the evaluator's own Σ|aᵢbᵢ|.
  The count and the magnitudes come from evaluating the builtin over ones and over the arguments' magnitudes, so no builtin's layout is written down.
- Measured on the Arc B580 against the interpreter, the most of `across` any element used beyond its range:
  `float_arithmetic` 3.9e-7 to 0, `matrices` 3.2e-5 to 1.6e-7, `vector_math` 1.9e-6 to 1.7e-7, `pixel_varyings` 1.1e-7 to 0.
  What remains there is `normalize`, `length` and `determinant`, a builtin's own precision.

## Open

- A product chain, `a * b * c`, which reassociates with a relative bound: not modeled.

- Whether `γ(n-1) · Σ|x_i|` is too wide to be useful where the terms cancel, which is where reassociation matters most.
  A dot product of four terms widens by 3 ulps of `Σ|x_i|`, so it should stay tight; a sum of 64 terms may not.
- A contraction inside a chain, `a * b + c * d` as `fma(a, b, c * d)`: the bound holds, but it should be checked against the exact-product rule so they compose rather than double-count.
