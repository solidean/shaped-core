---
created: 2026-11-03
priority: normal
parent: epic-tg-polynomials
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
