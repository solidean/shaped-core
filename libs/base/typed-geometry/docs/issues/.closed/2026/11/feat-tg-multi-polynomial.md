---
created: 2026-11-03
priority: next
parent: epic-tg-polynomials
closed: 2026-11-03
resolution: fixed
---

# tg::multi_polynomial<C, tg::vars<N, Order>>: sparse, statically indexed variables, exponents packed into words

Wanted:
- Terms sorted by the monomial order, exponents and coefficients in parallel arrays.
- `tg::vars<N, Order, Bits>`: N variables, each exponent a `Bits`-wide field of one or a few u64 words, with a guard bit.
- A term compare an integer compare, and a term product an integer add, its overflow checked.
- Orders `lex`, `grlex`, `grevlex` and `infinitesimal`: variable 0 the largest infinitesimal, every power of it larger than the next variable; the leading term is the dominant one.
- `sign()` under the infinitesimal order: the leading coefficient's.
- Ring operations, evaluation, substitution of a variable, derivative, degree per variable, `generators()`.
- Nesting: a `multi_polynomial` or `polynomial` coefficient, and `to_univariate(p, var)`.

Done:
- `symbolic/multi_polynomial.hh`, the four orders as field layouts plus an XOR mask, FLINT's way.
- Overflow fails through `CC_ASSERT_ALWAYS`.
- Tests check every order against its definition, the ring operations against evaluation at one and two words per monomial, and calculus by the product rule.
- The infinitesimal sign is checked against exact evaluation at x(i) = t^((D+1)^i) for a t small enough that the dominant term outweighs the rest.
- Speed is perf-tg-polynomial-multiplication's; the products sort every pair of terms for now.
