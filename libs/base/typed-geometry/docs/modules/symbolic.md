# Module: symbolic

> Module docs answer **"what belongs here?"** and **"why is it this way?"** — [docs/_index.md](../_index.md#module-docs) states the contract they follow.

## What this module is

`symbolic/` is exact algebra over the scalars: polynomials and rational polynomials.
It sits above `scalar/` and knows nothing of geometry; geometry may instantiate over its types, never the other way around.

## What belongs here

- `tg::polynomial<C, Degree>`: one variable, dense, the degree bounded inline or grown in a vector.
- `tg::multi_polynomial<C, Vars>`: several variables, sparse, the exponents packed into machine words.
- The monomial orders and variable sets `multi_polynomial` is generic over: `tg::vars<N, Order, Bits>`, and `tg::dynamic_vars<Order>` over `tg::symbol`s.
- `tg::rational_polynomial<P>`: a quotient of either, and a field.
- Algorithms on them: arithmetic, evaluation, composition, derivatives, division, gcds, resultants and real roots.

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

### Products: a dense box when it is small, a heap otherwise

A product's pairs are summed where its exponents' box is small, at most four slots per pair: Kronecker substitution.
Each term's exponents become one index into a thread-local accumulator, a pair lands at the sum of its factors' indices,
and the slots it touched are read out sorted by the order, so no slot outside the result is visited.
Past that, Monagan and Pearce's heap walks one row per term of the smaller factor, and the pairs come out greatest first, sorted without a sort.
Either way a coefficient is accumulated in place, through `assign_add_mul` where `C` has one, as `bigint` does.
Sums merge the sorted term lists, in place from the back when an operand is an rvalue, so `a * b + c * d` moves rather than copies.

Instructions retired on a Zen 5, from sorting every pair with plain `acc += a * b` to now:

| product | terms | before | now |
|---|---|---|---|
| orient3d over four points, each perturbed by an infinitesimal | 64 | 106,000 | 70,000 |
| Fateman's (1 + x + y + z + t)^10 squared | 10,626 | 568 M | 104 M |
| a sparse product in five variables, n = 6 | 114,000 | 286 M | 152 M |
| a univariate product of degree 100, two-limb coefficients | 201 | 3.08 M | 2.54 M |

What remains in the infinitesimal case is mostly allocation: every non-zero `bigint` coefficient is a node, and every polynomial two vectors.

### Small coefficients sum in words

A product of bigints whose coefficients all fit an i64, or one or two limbs, sums each output coefficient in a fixed-width accumulator:
three or five limbs of two's complement, one signed multiply and three adds a pair for i64s, no node until the sum is read out.
The polynomials find it by duck typing, `C::word_accumulator` and `C::template accumulator<W>`, and read the factors into word form once.
Against FLINT 3.3.1, built with MSVC, the univariate products of two-limb coefficients are level with it and the infinitesimal orient3d ahead,
Fateman's product is twice its instructions, and the sparse product is bound by node allocation (perf-cc-node-alloc-ring-walk).

### The gcd is the subresultant sequence, and the resultant shares its step

Over an exact field the gcd is Euclid's, made monic.
Over a ring with a gcd of its own, `bigint` or a polynomial over one, it is the subresultant sequence (Collins, Brown):
pseudo-remainders divided exactly by `g h^delta`, which keeps the coefficients as small as the determinants they are,
where the primitive sequence would take a gcd of every remainder's coefficients and plain Euclid's coefficients grow exponentially.
A polynomial over such a ring has this gcd itself, so nested polynomials have one: recursive, not the modular gcd a multivariate one wants.
Floats have none: their remainders never vanish.

The scale factors only change how large the coefficients grow on the way, never the normalized gcd, so no test of the gcd can see them.
`resultant` is the sequence's last value with every factor intact, so the step lives in one place for both,
and the resultant is tested against the Sylvester matrix's determinant, computed independently in rationals.

### Real roots: exact isolation, and floating-point roots on an interval

`isolate_real_roots` is Descartes' rule of signs with bisection (Vincent, Collins and Akritas) on the squarefree part, in exact integers:
Taylor shifts by one, reversals and scalings by powers of two, no division.
It returns one interval per distinct real root, open with dyadic endpoints, or a single point for a root met at a midpoint; `refine_root` narrows one.

`real_roots(p, lo, hi)` is for floating-point coefficients, curves and surfaces first.
The derivative's roots, found the same way, cut [lo, hi] into pieces on which p is monotone, and each piece with a sign change holds one root,
found by Newton's steps kept inside the bracket by bisection.
A fixed degree returns a `cc::fixed_vector`, so it never allocates.
A root of even multiplicity has no sign change; it is found only where p is exactly zero at a critical point.

### Named variables carry their own list

`multi_polynomial<C, dynamic_vars<Order>>` is the computer-algebra mode: variables are `tg::symbol`s, interned names compared by identity.
Each polynomial carries the sorted list of variables its terms are packed over, in the same 16-bit fields and under the same orders as `vars<>`.
Operands over different lists are repacked onto their union first; a variable with exponent zero everywhere ranks nothing, so no term moves.
The list is sorted by the names' bytes, not by the interned identity, so the variable order and the printed form are the same every run.
The width stays fixed and checked, as `vars<>`'s does, rather than growing: one representation to test, and 32767 is a large exponent.
Products go through the heap, there being no static box for the dense path; `parse` reads the printed form back.

### Rational polynomials reduce where a gcd exists

`rational_polynomial<P>` is a field, so a polynomial over it has `divrem`, which is what the trait seam was for.
Where `P` has a gcd, a univariate polynomial over `bigint` or an exact field, every operation reduces by it and the denominator leads positively,
monic over a field: the representation is canonical, and equality compares parts.
Where it has none, the multivariate types for now, the parts are kept as they come but for the sign, and equality cross-multiplies.
Symbolic perturbation needs no more there: the sign of a quotient of infinitesimal polynomials is the product of its parts' signs.
Evaluating needs the point in a field, so a rational polynomial over `bigint` is refused a `bigint` point at compile time instead of dividing with truncation.
