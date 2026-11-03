---
created: 2026-11-03
priority: next
---

# Polynomials in tg: univariate and multivariate, generic over the coefficient, with statically indexed variables at full speed

A `symbolic/` module, as structure.md plans it.
Two types, generic over the coefficient `C` by plain template duck typing:
- `tg::polynomial<C, Degree>`: one variable, dense, the degree fixed and inline or dynamic.
- `tg::multi_polynomial<C, Vars>`: sparse, the exponents packed into machine words; `Vars` a static set of N indexed variables or a dynamic one of interned symbols.

The algebra follows the coefficient: over a ring the polynomial is a ring, over a field it has `divrem`, and a polynomial is a valid coefficient, so nesting gives the recursive representation.
`tg::traits::is_field` says which coefficients divide; `bigint`'s `/` truncates, so the operator alone cannot.
Rational polynomials, a field, come later on top.

Uses:
- Evaluating a predicate under symbolic perturbation: a polynomial in K infinitesimals, K known statically, its sign the sign of the leading coefficient under the infinitesimal order.
- Root finding and fixed-degree polynomials for curves and surfaces.
- Computer algebra over named variables.

The order:
1. the univariate type and the coefficient traits;
2. the static multivariate type with its orders and sign;
3. dynamic variables, and speed against FLINT;
4. gcd, roots and rational polynomials.
