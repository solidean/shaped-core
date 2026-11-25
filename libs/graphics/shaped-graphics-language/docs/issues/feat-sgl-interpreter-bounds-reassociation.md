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

## Open

- Whether `γ(n-1) · Σ|x_i|` is too wide to be useful where the terms cancel, which is where reassociation matters most.
  A dot product of four terms widens by 3 ulps of `Σ|x_i|`, so it should stay tight; a sum of 64 terms may not.
- A contraction inside a chain, `a * b + c * d` as `fma(a, b, c * d)`: the bound holds, but it should be checked against the exact-product rule so they compose rather than double-count.
