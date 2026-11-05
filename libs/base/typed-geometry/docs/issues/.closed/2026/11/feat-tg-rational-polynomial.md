---
created: 2026-11-03
priority: normal
parent: epic-tg-polynomials
closed: 2026-11-03
resolution: fixed
---

# tg::rational_polynomial: quotients of polynomials, univariate and multivariate, a field

Wanted:
- A numerator and a denominator over either polynomial type, so `polynomial<rational_polynomial<...>>` has `divrem`.
- Whether it is reduced by default is open: reducing needs a multivariate gcd, while a sign needs only the parts' signs.

Done in `symbolic/rational_polynomial.hh`, the open question settled by duck typing:
reduced by the gcd where `P` has one, univariate today, and only sign-normalized where it has none, with equality by cross products.
The multivariate types become reduced once feat-tg-polynomial-gcd's multivariate gcd lands, with no change here.
