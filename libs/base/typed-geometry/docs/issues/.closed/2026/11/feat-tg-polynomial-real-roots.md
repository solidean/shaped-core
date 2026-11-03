---
created: 2026-11-03
priority: normal
parent: epic-tg-polynomials
closed: 2026-11-03
resolution: fixed
---

# Real roots of univariate polynomials: exact isolation and floating-point finding

Wanted:
- Exact real root isolation over `bigint` and `bigrat`: Descartes' rule of signs with bisection, or Sturm sequences.
- Floating-point roots for fixed low degrees, for curves and surfaces: closed forms where stable, bracketed Newton otherwise.

Done in `symbolic/polynomial_roots.hh`:
- `isolate_real_roots` and `refine_root`, Descartes with bisection in exact integers.
  Tested against Sturm's count on random polynomials, and against known rational roots with multiplicities.
- `real_roots(p, lo, hi)` over floats: the derivative's roots split the interval, bracketed Newton finds each root, inline for a fixed degree.
