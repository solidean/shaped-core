---
created: 2026-11-03
priority: next
parent: epic-tg-polynomials
closed: 2026-11-03
resolution: fixed
---

# tg::polynomial<C, Degree>: a univariate polynomial, dense, its degree fixed inline or dynamic

Wanted:
- Coefficients lowest first; `Degree = dynamic_degree` a trimmed `cc::vector`, a fixed `Degree` an inline array whose degree is a bound.
- Fixed degrees propagate: a product's bound is the sum of its factors', a derivative's one less.
- Ring operations, scalar products, equality, Horner evaluation at any value C multiplies with (a polynomial argument composes), derivative.
- `divrem` over a field, refused at compile time otherwise; `pseudo_divrem` over any ring.
- `tg::traits::is_field`, and `scalar_traits` for `bigint`, `bigrat` and the polynomials themselves, so they nest.

Done:
- `symbolic/polynomial.hh`, with `divrem` constrained on `tg::traits::is_field`, and `pseudo_divrem` and `div_exact` over rings.
- Division in place on a copy of the dividend, so a fixed bound never allocates.
- Tests check the ring operations against evaluation, division by its identity, the bounds' propagation by type, and nesting.
- Speed is perf-tg-polynomial-multiplication's.
