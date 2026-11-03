---
created: 2026-11-03
priority: normal
parent: epic-tg-polynomials
closed: 2026-11-03
resolution: fixed
---

# Polynomial gcd, content and primitive part

Wanted:
- Univariate: Euclid over a field, the subresultant sequence over a gcd domain, then a modular gcd once it is measured to win.
- Content and primitive part over `bigint`.
- Multivariate: a modular gcd (Brown, then Zippel for sparse inputs), the base of reduced rational polynomials.

Done, univariate:
- `gcd` by Euclid over exact fields and by the subresultant sequence over gcd rings, nested polynomials included.
- `content`, `primitive_part`, `squarefree_part`, and `resultant`, which shares the subresultant step and is tested against the Sylvester determinant.

Left: the multivariate modular gcd (Brown, then Zippel), and a modular univariate gcd if it measures faster.

Done, multivariate, for the static variables:
- `try_div_exact` and `div_exact`, by leading terms, bounded by the dividend's degrees so a local order cannot run on.
- `gcd`: the heuristic gcd over `bigint`, proved by division, ahead of FLINT's on (1 + x + y + z)^4 times a cofactor each;
  the recursive subresultant route behind it and for every other coefficient type.
The dynamic variables have no gcd yet, so their rational polynomials stay unreduced; a modular gcd waits until an input needs it.
