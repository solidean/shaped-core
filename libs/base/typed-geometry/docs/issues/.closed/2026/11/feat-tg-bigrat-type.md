---
created: 2026-11-03
priority: next
parent: epic-tg-bigrat
closed: 2026-11-03
resolution: fixed
---

# tg::bigrat and tg::bigrat_unreduced, with the TPI, orient3d and elimination benchmarks against mpq

Wanted:
- `tg::bigrat`: a numerator and a denominator, `bigint` each, the denominator positive and its zero value standing for 1.
- Sums by the gcd of the denominators only, products by the cross gcds, comparison by signs, then bit widths, then cross products.
- `assign_*` into kept results and operators that reuse an rvalue's nodes, as `bigint` has.
- `tg::bigrat_unreduced` for accumulating without a gcd per operation, reduced on conversion.
- Agreement tests against GMP's mpq, and counters against mpq and cpp_rational:
  three-plane intersection points as rationals with orient3d over them, and Gaussian elimination of small dense systems.

Done, measured on a Zen 5 in instructions retired against GMP 6.3's generic C and Boost's cpp_rational:

| workload | tg bigrat | tg unreduced | GMP mpq | Boost |
|---|---|---|---|---|
| three-plane intersection point, three reduced coordinates | 11,300 | - | 16,400 | 40,500 |
| orient3d over four such points | 330,000 | 62,000 | 464,000 | 971,000 |
| 8 x 8 Gaussian elimination | 852,000 | - | 1,080,000 | 3,365,000 |
| harmonic sum of 200 terms | 316,000 | 200,000 | 275,000 | 591,000 |

The harmonic sum, the stress test, is the one still behind; perf-bigrat-growing-denominators has it.
