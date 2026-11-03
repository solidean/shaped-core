#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <typed-geometry/scalar/traits.hh>
#include <typed-geometry/symbolic/fwd.hh>

#include <initializer_list>
#include <type_traits>

// tg::polynomial: a univariate polynomial over any coefficient type, its degree bounded inline or grown in a vector.
// libs/base/typed-geometry/docs/modules/symbolic.md has the design, and libs/base/typed-geometry/cheat-sheet.md the surface.

namespace tg::impl
{
template <class P, class C>
inline constexpr bool is_polynomial_over = false;
template <class C, int Degree>
inline constexpr bool is_polynomial_over<tg::polynomial<C, Degree>, C> = true;

template <class P>
inline constexpr bool is_polynomial = false;
template <class C, int Degree>
inline constexpr bool is_polynomial<tg::polynomial<C, Degree>> = true;

/// S multiplies and adds into polynomials over C as a constant: it converts to C and is no polynomial over C itself.
template <class S, class C>
concept polynomial_scalar_of = std::is_convertible_v<S const&, C> && !is_polynomial_over<S, C>;

[[nodiscard]] constexpr int polynomial_sum_degree(int a, int b)
{
    return a == dynamic_degree || b == dynamic_degree ? dynamic_degree : (a > b ? a : b);
}
[[nodiscard]] constexpr int polynomial_product_degree(int a, int b)
{
    return a == dynamic_degree || b == dynamic_degree ? dynamic_degree : a + b;
}
[[nodiscard]] constexpr int polynomial_derivative_degree(int a)
{
    return a == dynamic_degree ? dynamic_degree : (a > 0 ? a - 1 : 0);
}
/// A remainder's degree is below the divisor's, which is at most b.
[[nodiscard]] constexpr int polynomial_remainder_degree(int b)
{
    return b == dynamic_degree ? dynamic_degree : (b > 0 ? b - 1 : 0);
}

template <class C>
[[nodiscard]] bool coefficient_is_zero(C const& c)
{
    if constexpr (requires { tg::scalar_traits<C>::is_zero(c); })
        return tg::scalar_traits<C>::is_zero(c);
    else
        return c == C(0);
}

/// acc += a * b, without a temporary where C has a fused assign_add_mul.
template <class C>
void add_mul(C& acc, C const& a, C const& b)
{
    if constexpr (requires { acc.assign_add_mul(a, b); })
        acc.assign_add_mul(a, b);
    else
        acc += a * b;
}

/// acc -= a * b, without a temporary where C has a fused assign_sub_mul.
template <class C>
void sub_mul(C& acc, C const& a, C const& b)
{
    if constexpr (requires { acc.assign_sub_mul(a, b); })
        acc.assign_sub_mul(a, b);
    else
        acc -= a * b;
}

/// a / b where b divides a: a field's division, a ring's exact division where it has one, its truncating one otherwise.
template <class C>
[[nodiscard]] C exact_quotient(C const& a, C const& b)
{
    if constexpr (tg::traits::is_field<C>)
        return a / b;
    else if constexpr (requires { div_exact(a, b); })
        return div_exact(a, b);
    else
        return a / b;
}

template <class C>
[[nodiscard]] cc::string coefficient_string(C const& c)
{
    if constexpr (requires { c.to_string(); })
        return c.to_string();
    else
        return cc::format("{}", c);
}

template <class C, int N>
struct inline_coefficients
{
    C values[N] = {};
};
} // namespace tg::impl

/// c_0 + c_1 x + ... + c_d x^d over a coefficient type C, by duck typing: C needs + - * and construction from an int.
/// Over a ring it is a ring, and over a field (tg::traits::is_field) it has divrem; a polynomial is itself a valid C.
/// Degree = dynamic_degree holds the coefficients in a vector, trimmed after every operation.
/// A fixed Degree holds Degree + 1 coefficients inline and is a bound, not the degree: degree() is the highest non-zero one.
/// Fixed bounds propagate through the operators, a product's being the sum of its factors'.
template <class C, int Degree>
struct tg::polynomial
{
    static_assert(Degree >= 0 || Degree == dynamic_degree,
                  "tg::polynomial: Degree is a bound of at least 0, or dynamic_degree");

    using coefficient_t = C;
    static constexpr bool is_dynamic = Degree == dynamic_degree;
    static constexpr int degree_bound = Degree;

    polynomial() = default;

    /// The constant c.
    template <class S>
        requires impl::polynomial_scalar_of<S, C>
    polynomial(S const& c)
    {
        if constexpr (is_dynamic)
        {
            C v(c);
            if (!impl::coefficient_is_zero(v))
                _c.push_back(cc::move(v));
        }
        else
            _c.values[0] = C(c);
    }

    /// Coefficients lowest first: {1, 0, 3} is 1 + 3 x^2.
    polynomial(std::initializer_list<C> lowest_first)
      : polynomial(cc::span<C const>(lowest_first.begin(), isize(lowest_first.size())))
    {
    }

    /// Coefficients lowest first; a fixed Degree holds at most Degree + 1 of them, trailing zeros aside.
    explicit polynomial(cc::span<C const> lowest_first)
    {
        auto n = int(lowest_first.size());
        while (n > 0 && impl::coefficient_is_zero(lowest_first[n - 1]))
            --n;
        *this = zeroed(n);
        for (auto i = 0; i < n; ++i)
            (*this)[i] = lowest_first[i];
    }

    /// The same polynomial under another bound: implicit when it widens, explicit and asserted when it narrows.
    template <int D>
        requires(D != Degree)
    explicit(!is_dynamic && (D == dynamic_degree || D > Degree)) polynomial(polynomial<C, D> const& p)
    {
        auto const d = p.degree();
        *this = zeroed(d + 1);
        for (auto i = 0; i <= d; ++i)
            (*this)[i] = p[i];
    }

    /// c x^k.
    [[nodiscard]] static polynomial monomial(C c, int k)
    {
        CC_ASSERT(k >= 0, "tg::polynomial::monomial: the exponent must not be negative");
        if (impl::coefficient_is_zero(c))
            return {};
        auto p = zeroed(k + 1);
        p[k] = cc::move(c);
        return p;
    }

    /// The variable x.
    [[nodiscard]] static polynomial x() { return monomial(C(1), 1); }

    /// count stored coefficients, all zero, to be set through operator[] and trim()med: the building block of the algorithms.
    /// A fixed Degree stores Degree + 1 always, and count must not exceed that.
    [[nodiscard]] static polynomial zeroed(int count)
    {
        polynomial p;
        if constexpr (is_dynamic)
            p._c.resize_to_filled(count, C(0));
        else
            CC_ASSERT(count <= Degree + 1, "tg::polynomial: more coefficients than the fixed degree bound holds");
        return p;
    }

    // observers

    /// The highest power with a non-zero coefficient, -1 for the zero polynomial.
    [[nodiscard]] int degree() const
    {
        auto d = stored_count() - 1;
        while (d >= 0 && impl::coefficient_is_zero((*this)[d]))
            --d;
        return d;
    }
    [[nodiscard]] bool is_zero() const { return degree() < 0; }
    [[nodiscard]] bool is_constant() const { return degree() <= 0; }

    /// The stored coefficients, lowest first: degree() + 1 after a trim, Degree + 1 for a fixed bound.
    [[nodiscard]] cc::span<C const> coefficients() const { return cc::span<C const>(_data(), isize(stored_count())); }
    [[nodiscard]] int stored_count() const
    {
        if constexpr (is_dynamic)
            return int(_c.size());
        else
            return Degree + 1;
    }
    [[nodiscard]] C const& operator[](int i) const
    {
        CC_ASSERT(0 <= i && i < stored_count(), "tg::polynomial: coefficient index past the stored ones");
        return _data()[i];
    }
    [[nodiscard]] C& operator[](int i)
    {
        CC_ASSERT(0 <= i && i < stored_count(), "tg::polynomial: coefficient index past the stored ones");
        return _data()[i];
    }
    /// The coefficient of x^i, zero past the stored ones.
    [[nodiscard]] C coefficient(int i) const { return i < stored_count() ? (*this)[i] : C(0); }
    /// The coefficient of x^degree(); the polynomial must not be zero.
    [[nodiscard]] C const& leading_coefficient() const
    {
        auto const d = degree();
        CC_ASSERT(d >= 0, "tg::polynomial: the zero polynomial has no leading coefficient");
        return (*this)[d];
    }

    /// Drops the zero coefficients on top, which only a dynamic polynomial stores.
    void trim()
    {
        if constexpr (is_dynamic)
            while (!_c.empty() && impl::coefficient_is_zero(_c.back()))
                _c.remove_back();
    }

    /// The value at x by Horner's rule, in X where X is built from C (a bigint polynomial at a bigrat), in C otherwise.
    /// A polynomial over C composes instead: see tg::compose.
    template <class X>
        requires(!impl::is_polynomial_over<X, C>)
    [[nodiscard]] auto operator()(X const& x) const
    {
        using R = std::conditional_t<std::is_constructible_v<X, C const&>, X, C>;
        auto const d = degree();
        if (d < 0)
            return R(0);
        auto r = R((*this)[d]);
        for (auto i = d - 1; i >= 0; --i)
        {
            r = r * x;
            r += R((*this)[i]);
        }
        return r;
    }

    [[nodiscard]] cc::string to_string(cc::string_view var = "x") const;

    // arithmetic

    void negate()
    {
        for (auto i = 0; i < stored_count(); ++i)
            (*this)[i] = -(*this)[i];
    }

    template <int D>
        requires(is_dynamic || (D != dynamic_degree && D <= Degree))
    polynomial& operator+=(polynomial<C, D> const& b)
    {
        _accumulate(b, false);
        return *this;
    }
    template <int D>
        requires(is_dynamic || (D != dynamic_degree && D <= Degree))
    polynomial& operator-=(polynomial<C, D> const& b)
    {
        _accumulate(b, true);
        return *this;
    }
    template <class S>
        requires impl::polynomial_scalar_of<S, C>
    polynomial& operator+=(S const& s)
    {
        if (stored_count() == 0)
            *this = zeroed(1);
        (*this)[0] += C(s);
        trim();
        return *this;
    }
    template <class S>
        requires impl::polynomial_scalar_of<S, C>
    polynomial& operator-=(S const& s)
    {
        if (stored_count() == 0)
            *this = zeroed(1);
        (*this)[0] -= C(s);
        trim();
        return *this;
    }
    template <class S>
        requires impl::polynomial_scalar_of<S, C>
    polynomial& operator*=(S const& s)
    {
        C const f(s);
        for (auto i = 0; i < stored_count(); ++i)
            (*this)[i] *= f;
        trim();
        return *this;
    }
    template <int D>
        requires is_dynamic
    polynomial& operator*=(polynomial<C, D> const& b);

private:
    template <int D>
    void _accumulate(polynomial<C, D> const& b, bool subtract)
    {
        auto const n = b.degree() + 1;
        if constexpr (is_dynamic)
            if (stored_count() < n)
                _c.resize_to_filled(n, C(0));
        for (auto i = 0; i < n; ++i)
            if (subtract)
                (*this)[i] -= b[i];
            else
                (*this)[i] += b[i];
        trim();
    }

    [[nodiscard]] C const* _data() const
    {
        if constexpr (is_dynamic)
            return _c.data();
        else
            return _c.values;
    }
    [[nodiscard]] C* _data()
    {
        if constexpr (is_dynamic)
            return _c.data();
        else
            return _c.values;
    }

    std::conditional_t<is_dynamic, cc::vector<C>, impl::inline_coefficients<C, is_dynamic ? 1 : Degree + 1>> _c;
};

/// A quotient and a remainder, from tg::divrem.
template <class Q, class R>
struct tg::quotient_remainder
{
    Q quotient;
    R remainder;
};

/// lc(b)^power * a == quotient * b + remainder with deg(remainder) < deg(b), from tg::pseudo_divrem.
template <class P>
struct tg::pseudo_quotient_remainder
{
    P quotient;
    P remainder;
    int power = 0;
};

namespace tg
{
// equality is by value: bounds and stored zeros on top do not matter

template <class C, int A, int B>
[[nodiscard]] bool operator==(polynomial<C, A> const& a, polynomial<C, B> const& b)
{
    auto const d = a.degree();
    if (d != b.degree())
        return false;
    for (auto i = 0; i <= d; ++i)
        if (!(a[i] == b[i]))
            return false;
    return true;
}

template <class C, int D, class S>
    requires impl::polynomial_scalar_of<S, C>
[[nodiscard]] bool operator==(polynomial<C, D> const& a, S const& s)
{
    return a == polynomial<C, D>(s);
}

// ring operations

template <class C, int D>
[[nodiscard]] polynomial<C, D> operator-(polynomial<C, D> a)
{
    a.negate();
    return a;
}

template <class C, int A, int B>
[[nodiscard]] polynomial<C, impl::polynomial_sum_degree(A, B)> operator+(polynomial<C, A> const& a,
                                                                         polynomial<C, B> const& b)
{
    polynomial<C, impl::polynomial_sum_degree(A, B)> r(a);
    r += b;
    return r;
}

template <class C, int A, int B>
[[nodiscard]] polynomial<C, impl::polynomial_sum_degree(A, B)> operator-(polynomial<C, A> const& a,
                                                                         polynomial<C, B> const& b)
{
    polynomial<C, impl::polynomial_sum_degree(A, B)> r(a);
    r -= b;
    return r;
}

/// The schoolbook product, each coefficient accumulated in place.
template <class C, int A, int B>
[[nodiscard]] polynomial<C, impl::polynomial_product_degree(A, B)> operator*(polynomial<C, A> const& a,
                                                                             polynomial<C, B> const& b)
{
    using R = polynomial<C, impl::polynomial_product_degree(A, B)>;
    auto const da = a.degree();
    auto const db = b.degree();
    if (da < 0 || db < 0)
        return R();
    auto r = R::zeroed(da + db + 1);
    for (auto i = 0; i <= da; ++i)
    {
        if (impl::coefficient_is_zero(a[i]))
            continue;
        for (auto j = 0; j <= db; ++j)
            impl::add_mul(r[i + j], a[i], b[j]);
    }
    r.trim();
    return r;
}

template <class C, int D>
template <int B>
    requires polynomial<C, D>::is_dynamic
polynomial<C, D>& polynomial<C, D>::operator*=(polynomial<C, B> const& b)
{
    *this = *this * b;
    return *this;
}

template <class C, int D, class S>
    requires impl::polynomial_scalar_of<S, C>
[[nodiscard]] polynomial<C, D> operator+(polynomial<C, D> a, S const& s)
{
    a += s;
    return a;
}
template <class C, int D, class S>
    requires impl::polynomial_scalar_of<S, C>
[[nodiscard]] polynomial<C, D> operator+(S const& s, polynomial<C, D> a)
{
    a += s;
    return a;
}
template <class C, int D, class S>
    requires impl::polynomial_scalar_of<S, C>
[[nodiscard]] polynomial<C, D> operator-(polynomial<C, D> a, S const& s)
{
    a -= s;
    return a;
}
template <class C, int D, class S>
    requires impl::polynomial_scalar_of<S, C>
[[nodiscard]] polynomial<C, D> operator-(S const& s, polynomial<C, D> a)
{
    a.negate();
    a += s;
    return a;
}
template <class C, int D, class S>
    requires impl::polynomial_scalar_of<S, C>
[[nodiscard]] polynomial<C, D> operator*(polynomial<C, D> a, S const& s)
{
    a *= s;
    return a;
}
template <class C, int D, class S>
    requires impl::polynomial_scalar_of<S, C>
[[nodiscard]] polynomial<C, D> operator*(S const& s, polynomial<C, D> a)
{
    a *= s;
    return a;
}

/// a^e by repeated squaring.
template <class C, int D>
    requires(D == dynamic_degree)
[[nodiscard]] polynomial<C, D> pow(polynomial<C, D> a, int e)
{
    CC_ASSERT(e >= 0, "tg::pow of a polynomial: the exponent must not be negative");
    polynomial<C, D> r = C(1);
    while (e > 0)
    {
        if (e & 1)
            r *= a;
        e >>= 1;
        if (e > 0)
            a *= a;
    }
    return r;
}

// calculus and composition

template <class C, int D>
[[nodiscard]] polynomial<C, impl::polynomial_derivative_degree(D)> derivative(polynomial<C, D> const& p)
{
    using R = polynomial<C, impl::polynomial_derivative_degree(D)>;
    auto const d = p.degree();
    if (d <= 0)
        return R();
    auto r = R::zeroed(d);
    for (auto i = 1; i <= d; ++i)
        r[i - 1] = p[i] * C(i);
    r.trim();
    return r;
}

/// p(q), by Horner's rule over polynomials.
template <class C, int P, int Q>
[[nodiscard]] auto compose(polynomial<C, P> const& p, polynomial<C, Q> const& q)
{
    constexpr auto degree = P == dynamic_degree || Q == dynamic_degree ? dynamic_degree : P * Q;
    auto const d = p.degree();
    polynomial<C> r;
    if (d >= 0)
    {
        r = p[d];
        for (auto i = d - 1; i >= 0; --i)
        {
            r = r * q;
            r += p[i];
        }
    }
    if constexpr (degree == dynamic_degree)
        return r;
    else
        return polynomial<C, degree>(r);
}

// division

namespace impl
{
/// Divides the n + 1 coefficients w by the m + 1 coefficients b in place, b[m] != 0 and n >= m:
/// afterwards w[0, m) holds the remainder and w[m, n] the quotient.
/// b[m] must divide each leading coefficient on the way: C a field, or the division known to be exact.
template <class C>
void divide_in_place(C* w, int n, C const* b, int m)
{
    for (auto k = n - m; k >= 0; --k)
    {
        auto t = exact_quotient(w[m + k], b[m]);
        for (auto j = 0; j < m; ++j)
            sub_mul(w[j + k], t, b[j]);
        w[m + k] = cc::move(t);
    }
}

/// As divide_in_place, but over a ring: afterwards lc^(n - m + 1) times the dividend is w[m, n] * b + w[0, m), lc = b[m].
template <class C>
void pseudo_divide_in_place(C* w, int n, C const* b, int m)
{
    auto const& lc = b[m];
    for (auto k = n - m; k >= 0; --k)
    {
        auto t = w[m + k];
        // q = lc q + t x^k and r = lc r - t x^k b, the quotient so far sitting above m + k
        for (auto i = m + k + 1; i <= n; ++i)
            w[i] *= lc;
        for (auto j = 0; j < m + k; ++j)
            w[j] *= lc;
        for (auto j = 0; j < m; ++j)
            sub_mul(w[j + k], t, b[j]);
        w[m + k] = cc::move(t);
    }
}

template <class P, class C>
[[nodiscard]] P polynomial_from_range(C* w, int begin, int end)
{
    auto p = P::zeroed(end - begin);
    for (auto i = begin; i < end; ++i)
        p[i - begin] = cc::move(w[i]);
    p.trim();
    return p;
}
} // namespace impl

/// a = quotient * b + remainder with deg(remainder) < deg(b), over a field; b must not be zero.
/// Over a ring there is none, since the quotient's coefficients leave it: pseudo_divrem or div_exact instead.
/// A fixed bound keeps its quotient at a's bound, since b's actual degree may lie below its own.
template <class C, int A, int B>
[[nodiscard]] quotient_remainder<polynomial<C, A>, polynomial<C, impl::polynomial_remainder_degree(B)>> divrem(
    polynomial<C, A> const& a,
    polynomial<C, B> const& b)
    requires traits::is_field<C>
{
    using R = polynomial<C, impl::polynomial_remainder_degree(B)>;
    auto const n = a.degree();
    auto const m = b.degree();
    CC_ASSERT(m >= 0, "tg::divrem: division by the zero polynomial");
    if (n < m)
        return {.quotient = {}, .remainder = R(a)};
    auto w = a;
    impl::divide_in_place(&w[0], n, &b[0], m);
    return {.quotient = impl::polynomial_from_range<polynomial<C, A>>(&w[0], m, n + 1),
            .remainder = impl::polynomial_from_range<R>(&w[0], 0, m)};
}

/// lc(b)^power * a = quotient * b + remainder with deg(remainder) < deg(b) and power = max(deg(a) - deg(b) + 1, 0), over any ring.
template <class C, int A, int B>
[[nodiscard]] pseudo_quotient_remainder<polynomial<C, A>> pseudo_divrem(polynomial<C, A> const& a,
                                                                        polynomial<C, B> const& b)
{
    auto const n = a.degree();
    auto const m = b.degree();
    CC_ASSERT(m >= 0, "tg::pseudo_divrem: division by the zero polynomial");
    if (n < m)
        return {.quotient = {}, .remainder = a, .power = 0};
    auto w = a;
    impl::pseudo_divide_in_place(&w[0], n, &b[0], m);
    return {.quotient = impl::polynomial_from_range<polynomial<C, A>>(&w[0], m, n + 1),
            .remainder = impl::polynomial_from_range<polynomial<C, A>>(&w[0], 0, m),
            .power = n - m + 1};
}

/// a / b where b divides a, over a ring whose quotients are exact (bigint) or a field.
template <class C, int A, int B>
[[nodiscard]] polynomial<C, A> div_exact(polynomial<C, A> const& a, polynomial<C, B> const& b)
{
    auto const n = a.degree();
    auto const m = b.degree();
    CC_ASSERT(m >= 0, "tg::div_exact: division by the zero polynomial");
    if (n < 0)
        return {};
    CC_ASSERT(n >= m, "tg::div_exact: the divisor does not divide");
    auto w = a;
    impl::divide_in_place(&w[0], n, &b[0], m);
    for (auto j = 0; j < m; ++j)
        CC_ASSERT(impl::coefficient_is_zero(w[j]), "tg::div_exact: the divisor does not divide");
    return impl::polynomial_from_range<polynomial<C, A>>(&w[0], m, n + 1);
}

// content, primitive part and gcd

namespace impl
{
/// C has a gcd: bigint does, and a polynomial over such a C, through tg::gcd below.
template <class C>
concept has_gcd = requires(C const& a) { gcd(a, a); };

/// -1, 0 or +1 for a coefficient: its own sign() where it has one, a polynomial's leading coefficient's, its comparison with 0 otherwise.
template <class C>
[[nodiscard]] int leading_sign(C const& c)
{
    if constexpr (requires { c.sign(); })
        return c.sign();
    else if constexpr (is_polynomial<C>)
        return c.is_zero() ? 0 : leading_sign(c.leading_coefficient());
    else
        return int(C(0) < c) - int(c < C(0));
}

/// p with every coefficient divided by c, which divides each of them.
template <class C, int D>
[[nodiscard]] polynomial<C, D> divide_coefficients(polynomial<C, D> p, C const& c)
{
    for (auto i = 0; i < p.stored_count(); ++i)
        if (!coefficient_is_zero(p[i]))
            p[i] = exact_quotient(p[i], c);
    return p;
}

template <class C>
[[nodiscard]] C power_of(C const& c, int e)
{
    auto r = C(1);
    for (auto i = 0; i < e; ++i)
        r = r * c;
    return r;
}

/// The subresultant sequence's scale factors (Collins, Brown): g the last leading coefficient, h the last subresultant's.
template <class C>
struct subresultant_scale
{
    C g = C(1);
    C h = C(1);
};

/// One step of the sequence, from the pseudo-remainder r of x by y with delta = deg x - deg y:
/// x takes y, and y takes r / (g h^delta), which is exact; then g and h move on, h to g^delta / h^(delta - 1), also exact.
template <class C>
void subresultant_step(polynomial<C>& x, polynomial<C>& y, polynomial<C> r, int delta, subresultant_scale<C>& s)
{
    x = cc::move(y);
    y = divide_coefficients(cc::move(r), s.g * power_of(s.h, delta));
    s.g = x.leading_coefficient();
    if (delta == 1)
        s.h = s.g;
    else if (delta > 1)
        s.h = exact_quotient(power_of(s.g, delta), power_of(s.h, delta - 1));
}
} // namespace impl

/// The gcd of p's coefficients, signed as p's leading coefficient, so that primitive_part(p) leads positively; 0 for p = 0.
template <class C, int D>
    requires impl::has_gcd<C>
[[nodiscard]] C content(polynomial<C, D> const& p)
{
    auto g = C(0);
    for (auto i = 0; i < p.stored_count(); ++i)
        if (!impl::coefficient_is_zero(p[i]))
            g = gcd(g, p[i]);
    if (!p.is_zero() && impl::leading_sign(p.leading_coefficient()) < 0)
        g = -g;
    return g;
}

/// p / content(p): coefficients without a common factor, the leading one positive.
template <class C, int D>
    requires impl::has_gcd<C>
[[nodiscard]] polynomial<C, D> primitive_part(polynomial<C, D> const& p)
{
    if (p.is_zero())
        return p;
    return impl::divide_coefficients(p, content(p));
}

/// The greatest common divisor, unique up to a unit and normalized: monic over a field, leading positively over the integers.
/// Over an exact field it is Euclid's; over a ring with a gcd of its own, the subresultant sequence, whose divisions are exact
/// and whose coefficients stay as small as the determinants they are (Collins, Brown).
/// A polynomial over such a ring has this gcd, so nested polynomials have one too; floats have none, their remainders never vanish.
template <class C, int A, int B>
    requires((traits::is_field<C> && traits::is_exact<C>) || impl::has_gcd<C>)
[[nodiscard]] polynomial<C> gcd(polynomial<C, A> const& a, polynomial<C, B> const& b)
{
    using P = polynomial<C>;
    P x(a);
    P y(b);
    if (x.degree() < y.degree())
        cc::swap(x, y);
    if constexpr (traits::is_field<C> && traits::is_exact<C>)
    {
        while (!y.is_zero())
        {
            P r(divrem(x, y).remainder);
            x = cc::move(y);
            y = cc::move(r);
        }
        if (x.is_zero())
            return x;
        auto const lc = x.leading_coefficient();
        return impl::divide_coefficients(x, lc);
    }
    else
    {
        if (y.is_zero())
            return impl::leading_sign(x.is_zero() ? C(0) : x.leading_coefficient()) < 0 ? -x : x;
        auto const c = gcd(content(x), content(y));
        x = primitive_part(x);
        y = primitive_part(y);
        impl::subresultant_scale<C> scale;
        while (true)
        {
            auto const delta = x.degree() - y.degree();
            auto r = pseudo_divrem(x, y).remainder;
            if (r.is_zero())
                break;
            if (r.degree() == 0)
            {
                y = P(C(1));
                break;
            }
            impl::subresultant_step(x, y, cc::move(r), delta, scale);
        }
        return primitive_part(y) * c;
    }
}

/// The resultant: the determinant of the Sylvester matrix, zero exactly when a and b share a root.
/// By the subresultant sequence, over an exact field or a ring with a gcd, its divisions exact (Cohen, Algorithm 3.3.7).
template <class C, int A, int B>
    requires((traits::is_field<C> && traits::is_exact<C>) || impl::has_gcd<C>)
[[nodiscard]] C resultant(polynomial<C, A> const& a, polynomial<C, B> const& b)
{
    using P = polynomial<C>;
    P x(a);
    P y(b);
    if (x.is_zero() || y.is_zero())
        return C(0);
    // res(a, b) = (-1)^(deg a deg b) res(b, a)
    auto sign = 1;
    if (x.degree() < y.degree())
    {
        cc::swap(x, y);
        if (x.degree() % 2 == 1 && y.degree() % 2 == 1)
            sign = -1;
    }
    // res(c p, d q) = c^deg q d^deg p res(p, q): contents out first where there are any
    auto t = C(1);
    if constexpr (!traits::is_field<C>)
    {
        auto const cx = content(x);
        auto const cy = content(y);
        t = impl::power_of(cx, y.degree()) * impl::power_of(cy, x.degree());
        x = impl::divide_coefficients(x, cx);
        y = impl::divide_coefficients(y, cy);
    }
    impl::subresultant_scale<C> scale;
    while (y.degree() > 0)
    {
        auto const delta = x.degree() - y.degree();
        if (x.degree() % 2 == 1 && y.degree() % 2 == 1)
            sign = -sign;
        auto r = pseudo_divrem(x, y).remainder;
        impl::subresultant_step(x, y, cc::move(r), delta, scale);
    }
    if (y.is_zero())
        return C(0);
    // the last subresultant: h^(1 - deg x) lc(y)^deg x, exact
    auto const d = x.degree();
    auto const last = impl::exact_quotient(impl::power_of(y.leading_coefficient(), d), impl::power_of(scale.h, d - 1));
    return sign < 0 ? -(t * last) : t * last;
}

/// p without repeated factors: p / gcd(p, p'), up to a unit.
template <class C, int D>
    requires((traits::is_field<C> && traits::is_exact<C>) || impl::has_gcd<C>)
[[nodiscard]] polynomial<C> squarefree_part(polynomial<C, D> const& p)
{
    if (p.degree() <= 0)
        return polynomial<C>(p);
    return div_exact(polynomial<C>(p), gcd(p, derivative(p)));
}
} // namespace tg

/// Highest power first, as "3*x^2 - x + 1"; a coefficient that is itself a sum is parenthesized where a power follows it.
template <class C, int Degree>
cc::string tg::polynomial<C, Degree>::to_string(cc::string_view var) const
{
    auto const d = degree();
    if (d < 0)
        return "0";
    cc::string s;
    for (auto i = d; i >= 0; --i)
    {
        auto const& c = (*this)[i];
        if (impl::coefficient_is_zero(c))
            continue;
        auto cs = impl::coefficient_string(c);
        auto const body = cc::string_view(cs).subview(cs.starts_with("-") ? 1 : 0);
        if (i > 0 && (body.contains(" + ") || body.contains(" - ")))
            cs = "(" + cs + ")";
        auto const negative = cs.starts_with("-");
        auto term = cc::string(cc::string_view(cs).subview(negative ? 1 : 0));
        if (i > 0)
        {
            if (term == "1")
                term = "";
            else
                term += "*";
            term += var;
            if (i > 1)
                term += cc::format("^{}", i);
        }
        if (s.empty())
            s = negative ? "-" + term : term;
        else
            s += (negative ? " - " : " + ") + term;
    }
    return s;
}

/// A polynomial is a scalar to tg: exact when its coefficients are, never a field, and a coefficient of another polynomial.
template <class C, int Degree>
struct tg::scalar_traits<tg::polynomial<C, Degree>>
{
    using T = tg::polynomial<C, Degree>;

    static constexpr bool has_sqrt = false;
    static constexpr bool has_trigonometry = false;
    static constexpr bool has_exponential = false;
    static constexpr bool has_rounding = false;
    static constexpr bool has_abs = false;
    static constexpr bool has_pow2 = false;
    static constexpr bool has_non_finite = false;
    static constexpr bool is_exact = tg::traits::is_exact<C>;
    static constexpr bool is_field = false;

    [[nodiscard]] static T one() { return T(C(1)); }
    [[nodiscard]] static bool is_zero(T const& x) { return x.is_zero(); }
    [[nodiscard]] static bool is_one(T const& x)
    {
        return x.degree() == 0 && tg::impl::coefficient_is_zero(x[0] - C(1));
    }
};
