#pragma once

#include <typed-geometry/scalar/fwd.hh>

namespace tg
{
//
// Symbolic algebra
//

/// The Degree of a tg::polynomial whose coefficients are held in a growable vector rather than inline.
inline constexpr int dynamic_degree = -1;

/// a univariate polynomial over coefficients C, lowest first: Degree bounds it inline, or dynamic_degree grows it
/// (see symbolic/polynomial.hh).
template <class C, int Degree = dynamic_degree>
struct polynomial;

/// monomial orders of a tg::vars (see symbolic/multi_polynomial.hh).
struct lex_order;
struct grlex_order;
struct grevlex_order;
/// the variables as infinitesimals, each power of x(i) dominating x(i + 1): the order whose leading coefficient is the sign.
struct infinitesimal_order;

/// N statically indexed variables, exponents packed Bits wide and ordered by Order (see symbolic/multi_polynomial.hh).
template <int N, class Order = grevlex_order, int Bits = 16>
struct vars;

/// a named variable of a computer-algebra polynomial, interned (see symbolic/symbol.hh).
struct symbol;

/// a value for a variable, to evaluate or substitute by (see symbolic/symbol.hh).
template <class X>
struct assignment;

/// variables known only at run time, as symbols, each polynomial carrying its own (see symbolic/dynamic_polynomial.hh).
template <class Order = grevlex_order>
struct dynamic_vars;

/// a sparse polynomial over coefficients C in the variables Vars (see symbolic/multi_polynomial.hh).
template <class C, class Vars>
struct multi_polynomial;

/// an interval holding exactly one real root of a polynomial, or the root itself (see symbolic/polynomial_roots.hh).
struct root_interval;

/// a quotient of two polynomials of type P, a field (see symbolic/rational_polynomial.hh).
template <class P>
struct rational_polynomial;

/// a quotient and a remainder, as tg::divrem of polynomials returns them.
template <class Q, class R = Q>
struct quotient_remainder;

/// lc(b)^k * a == quotient * b + remainder, as tg::pseudo_divrem returns it, with k = deg(a) - deg(b) + 1.
template <class P>
struct pseudo_quotient_remainder;
} // namespace tg
