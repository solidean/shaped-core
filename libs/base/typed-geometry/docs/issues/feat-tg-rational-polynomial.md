---
created: 2026-11-03
priority: normal
parent: epic-tg-polynomials
---

# tg::rational_polynomial: quotients of polynomials, univariate and multivariate, a field

Wanted:
- A numerator and a denominator over either polynomial type, so `polynomial<rational_polynomial<...>>` has `divrem`.
- Whether it is reduced by default is open: reducing needs a multivariate gcd, while a sign needs only the parts' signs.
