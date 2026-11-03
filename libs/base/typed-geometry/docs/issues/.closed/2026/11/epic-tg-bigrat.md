---
created: 2026-11-03
priority: next
closed: 2026-11-03
resolution: fixed
---

# tg::bigrat: exact rationals on bigint, reduced by default, at GMP's mpq or faster

A rational over two bigints, the denominator's null meaning 1, so an integer-valued rational costs what a bigint does.
Reduced after every operation, as GMP's mpq is; `bigrat_unreduced` is the escape hatch for hot loops, reduced once at the end.
Measured against GMP's mpq and Boost's cpp_rational on three-plane intersection points with orient3d over them, and on small Gaussian eliminations.

The order: a fast gcd first, since reducing decides a rational's speed, then the type and the benchmarks.
