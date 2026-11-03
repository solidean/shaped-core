#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/string.hh>
#include <typed-geometry/scalar/traits.hh>
#include <typed-geometry/symbolic/fwd.hh>
#include <typed-geometry/symbolic/polynomial.hh>

#include <type_traits>
#include <utility>

// tg::rational_polynomial: a quotient of two polynomials, univariate or multivariate, and a field.
// libs/base/typed-geometry/docs/modules/symbolic.md has the design, and libs/base/typed-geometry/cheat-sheet.md the surface.

namespace tg::impl
{
template <class R>
inline constexpr bool is_rational_polynomial = false;
template <class P>
inline constexpr bool is_rational_polynomial<tg::rational_polynomial<P>> = true;

/// S builds a rational polynomial over P as its numerator: a scalar of P's, or P itself.
template <class S, class P>
concept rational_polynomial_numerator_of = std::is_convertible_v<S const&, P> && !is_rational_polynomial<S>;
} // namespace tg::impl

/// numerator / denominator over a polynomial type P closed under its operators: tg::polynomial<C> or a tg::multi_polynomial.
/// It is a field, so a polynomial over it has divrem, as tg::traits::is_field says.
/// Where P has a gcd (a univariate polynomial over bigint or an exact field), every operation reduces by it,
/// and the denominator leads positively, monic over a field: equal values have equal parts.
/// Where it has none (the multivariate types, whose gcd is to come), the parts are kept as they come but for the sign,
/// the denominator's leading coefficient positive, and equality cross-multiplies; a sign needs only the parts' signs.
template <class P>
struct tg::rational_polynomial
{
    static_assert(std::is_same_v<decltype(std::declval<P const&>() * std::declval<P const&>()), P>,
                  "tg::rational_polynomial: P must be closed under *, a dynamic-degree polynomial or a "
                  "multi_polynomial");

    using polynomial_t = P;
    using coefficient_t = typename P::coefficient_t;
    static constexpr bool is_reduced = impl::has_gcd<P>;

    rational_polynomial() : _den(coefficient_t(1)) {}

    /// The polynomial or constant n, over 1.
    template <class S>
        requires impl::rational_polynomial_numerator_of<S, P>
    rational_polynomial(S const& n) : _num(n), _den(coefficient_t(1))
    {
    }

    /// num / den, den != 0.
    rational_polynomial(P num, P den) : _num(cc::move(num)), _den(cc::move(den))
    {
        CC_ASSERT(!_den.is_zero(), "tg::rational_polynomial: a zero denominator");
        _normalize();
    }

    [[nodiscard]] P const& numerator() const { return _num; }
    [[nodiscard]] P const& denominator() const { return _den; }
    [[nodiscard]] bool is_zero() const { return _num.is_zero(); }

    /// The sign over infinitesimal variables: the numerator's times the denominator's.
    [[nodiscard]] int sign() const
        requires requires(P const& p) { p.sign(); }
    {
        return _num.sign() * _den.sign();
    }

    /// The value at a point, numerator's over denominator's, which needs the point's scalar to be a field:
    /// a rational polynomial over bigint takes bigrat values, never bigint ones, whose division truncates.
    template <class... X>
    [[nodiscard]] auto operator()(X const&... x) const
    {
        auto n = _num(x...);
        static_assert(traits::is_field<decltype(n)>, "tg::rational_polynomial: evaluate at a field's values, a bigrat "
                                                     "rather than a bigint");
        return n / _den(x...);
    }

    [[nodiscard]] cc::string to_string() const
    {
        if (_den == P(coefficient_t(1)))
            return impl::coefficient_string(_num);
        return "(" + impl::coefficient_string(_num) + ")/(" + impl::coefficient_string(_den) + ")";
    }

    // arithmetic

    void negate() { _num.negate(); }
    [[nodiscard]] friend rational_polynomial operator-(rational_polynomial a)
    {
        a.negate();
        return a;
    }
    [[nodiscard]] friend rational_polynomial operator+(rational_polynomial const& a, rational_polynomial const& b)
    {
        return {a._num * b._den + b._num * a._den, a._den * b._den};
    }
    [[nodiscard]] friend rational_polynomial operator-(rational_polynomial const& a, rational_polynomial const& b)
    {
        return {a._num * b._den - b._num * a._den, a._den * b._den};
    }
    [[nodiscard]] friend rational_polynomial operator*(rational_polynomial const& a, rational_polynomial const& b)
    {
        return {a._num * b._num, a._den * b._den};
    }
    /// a / b, b != 0.
    [[nodiscard]] friend rational_polynomial operator/(rational_polynomial const& a, rational_polynomial const& b)
    {
        CC_ASSERT(!b.is_zero(), "tg::rational_polynomial: division by zero");
        return {a._num * b._den, a._den * b._num};
    }
    rational_polynomial& operator+=(rational_polynomial const& b) { return *this = *this + b; }
    rational_polynomial& operator-=(rational_polynomial const& b) { return *this = *this - b; }
    rational_polynomial& operator*=(rational_polynomial const& b) { return *this = *this * b; }
    rational_polynomial& operator/=(rational_polynomial const& b) { return *this = *this / b; }

    /// Reduced, the parts compare; otherwise the cross products do.
    [[nodiscard]] friend bool operator==(rational_polynomial const& a, rational_polynomial const& b)
    {
        if constexpr (is_reduced)
            return a._num == b._num && a._den == b._den;
        else
            return a._num * b._den == b._num * a._den;
    }

private:
    void _normalize()
    {
        if (_num.is_zero())
        {
            _den = P(coefficient_t(1));
            return;
        }
        if constexpr (is_reduced)
        {
            auto const g = gcd(_num, _den);
            if (!(g == P(coefficient_t(1))))
            {
                _num = div_exact(_num, g);
                _den = div_exact(_den, g);
            }
        }
        if (impl::leading_sign(_den.leading_coefficient()) < 0)
        {
            _num.negate();
            _den.negate();
        }
        // over a field the denominator is made monic, so equal values have equal parts there too
        if constexpr (is_reduced && traits::is_field<coefficient_t>)
        {
            auto const lc = _den.leading_coefficient();
            if (!traits::is_one(lc))
            {
                _num = impl::divide_coefficients(_num, lc);
                _den = impl::divide_coefficients(_den, lc);
            }
        }
    }

    P _num;
    P _den;
};

/// A rational polynomial is a field to tg, which is what lets a polynomial over it divide.
template <class P>
struct tg::scalar_traits<tg::rational_polynomial<P>>
{
    using T = tg::rational_polynomial<P>;

    static constexpr bool has_sqrt = false;
    static constexpr bool has_trigonometry = false;
    static constexpr bool has_exponential = false;
    static constexpr bool has_rounding = false;
    static constexpr bool has_abs = false;
    static constexpr bool has_pow2 = false;
    static constexpr bool has_non_finite = false;
    static constexpr bool is_exact = tg::traits::is_exact<P>;
    static constexpr bool is_field = true;

    [[nodiscard]] static T one() { return T(P(typename P::coefficient_t(1))); }
    [[nodiscard]] static bool is_zero(T const& x) { return x.is_zero(); }
    [[nodiscard]] static bool is_one(T const& x) { return x == one(); }
};
