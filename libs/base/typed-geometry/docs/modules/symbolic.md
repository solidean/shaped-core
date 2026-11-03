# Module: symbolic

> Module docs answer **"what belongs here?"** and **"why is it this way?"** — [docs/_index.md](../_index.md#module-docs) states the contract they follow.

## What this module is

`symbolic/` is exact algebra over the scalars: polynomials today, rational polynomials next.
It sits above `scalar/` and knows nothing of geometry; geometry may instantiate over its types, never the other way around.

## What belongs here

- `tg::polynomial<C, Degree>`: one variable, dense, the degree bounded inline or grown in a vector.
- `tg::multi_polynomial<C, Vars>`: several variables, sparse, the exponents packed into machine words.
- The monomial orders and variable sets `multi_polynomial` is generic over: `tg::vars<N, Order, Bits>` today, interned symbols next.
- Algorithms on them: arithmetic, evaluation, composition, derivatives, division; gcds, real roots and rational polynomials as they land.

## What does NOT belong here

- Scalars that are one number, `bigint` and `bigrat` included: those are `scalar/`.
- Curves and surfaces: they are geometry, and hold polynomials rather than being them.

## Key decisions

### The algebra follows the coefficient, by duck typing

A polynomial over `C` needs only what it uses: `+ - *` and construction from an int for the ring operations.
Over a ring it is a ring, and a polynomial is itself a valid `C`, so nesting gives the recursive representation for free:
`polynomial<polynomial<bigint>>` is two variables, and `multi_polynomial<multi_polynomial<...>, vars<K, infinitesimal_order>>` is infinitesimals over inputs.

Division is the exception, because the operator cannot say whether it inverts multiplication:
`bigint`'s `/` truncates, so a `divrem` written against `/` would compile over the integers and be wrong.
`tg::traits::is_field<C>` says it, in `scalar_traits` like every other capability: the floats and `bigrat` are fields, the integers and polynomials are not.
`divrem` requires a field; over a ring, `pseudo_divrem` scales the dividend by a power of the leading coefficient instead, and `div_exact` divides when the division is known to be exact.

### A fixed degree is an inline bound

`polynomial<f64, 3>` holds four coefficients inline and never allocates, for curves, surfaces and root finding.
The bound is not the degree: `degree()` is the highest non-zero coefficient, so a cubic bound may hold a line.
Bounds propagate through the operators, a product's being the sum of its factors', so `polynomial<f64, 3> * polynomial<f64, 2>` is a `polynomial<f64, 5>`.
Division works in place on a copy of the dividend's coefficients, the remainder below the divisor's degree and the quotient above it, so it stays inline too.
`dynamic_degree` keeps the coefficients in a vector, trimmed after every operation.

### Packed exponents, one integer compare per word

A `multi_polynomial` keeps its terms sorted by the monomial order, exponents and coefficients in two parallel arrays.
`vars<N, Order, Bits>` packs a term's exponents into `Bits`-wide fields of one or a few u64 words, as FLINT's `mpoly` does:
multiplying two terms is adding their words, and comparing them is an XOR with a per-order mask and an integer compare per word.
The mask is what lets one layout serve every order: grevlex and the infinitesimal order compare their variable fields reversed, yet still multiply by adding.
Each field's top bit is a guard: a product whose exponent overflows sets it, and the check fails in every build, not only with assertions on.

### The infinitesimal order makes the sign the leading coefficient's

Symbolic perturbation evaluates a predicate with infinitesimals ε0 >> ε1 >> ...: every power of ε(i) dominates ε(i + 1), and 1 dominates them all.
`infinitesimal_order` ranks monomials that way, the exponent of the last variable deciding first and the smaller one winning, so the leading term is the dominant one.
The sign of a polynomial in infinitesimals is then its leading coefficient's, which `sign()` returns.
1 being the largest monomial makes it a local order rather than a well-order: it ranks terms for signs, and division algorithms that need a well-order use the others.

### Not yet measured

Products sort all pairs of terms and accumulate each run of equal monomials in place; that is the correct first version, not the fast one.
Kronecker substitution into a dense accumulator, Monagan and Pearce's heap multiplication, and a fused `bigint::assign_add_mul` are perf-tg-polynomial-multiplication's, measured against FLINT.
