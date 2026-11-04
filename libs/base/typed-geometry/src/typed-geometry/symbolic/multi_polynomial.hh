#pragma once

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <typed-geometry/scalar/bigint/bigint.hh>
#include <typed-geometry/scalar/traits.hh>
#include <typed-geometry/symbolic/fwd.hh>
#include <typed-geometry/symbolic/polynomial.hh>

#include <type_traits>

// tg::multi_polynomial: a sparse polynomial in several variables over any coefficient type, exponents packed into words.
// libs/base/typed-geometry/docs/modules/symbolic.md has the design, and libs/base/typed-geometry/cheat-sheet.md the surface.

// The monomial orders.
// Each says how a vars<> lays out its exponent fields, most significant first, and which fields it compares reversed:
// comparing two terms is then one XOR and one integer compare per word, while multiplying them stays one add.

/// x0 > x1 > ... lexicographically: the exponent of x0 decides first.
struct tg::lex_order
{
    static constexpr bool graded = false;
    static constexpr bool variables_reversed = false;
    static constexpr bool compare_reversed = false;
};

/// Total degree first, then lex.
struct tg::grlex_order
{
    static constexpr bool graded = true;
    static constexpr bool variables_reversed = false;
    static constexpr bool compare_reversed = false;
};

/// Total degree first, then the smaller exponent of the last variable wins: the usual order for Gröbner bases.
struct tg::grevlex_order
{
    static constexpr bool graded = true;
    static constexpr bool variables_reversed = true;
    static constexpr bool compare_reversed = true;
};

/// The variables as infinitesimals, 1 >> x0 >> x0^2 >> ... >> x1 >> ...: every power of x(i) dominates x(i + 1).
/// The leading term is the dominant one, the constant first, so a polynomial's sign is its leading coefficient's.
/// 1 is the largest monomial, which makes it a local order, not a well-order: it ranks terms for signs, not for division.
struct tg::infinitesimal_order
{
    static constexpr bool graded = false;
    static constexpr bool variables_reversed = true;
    static constexpr bool compare_reversed = true;
};

namespace tg::impl
{
/// A monomial's exponents packed into W words, fields most significant first; it multiplies by adding.
template <int W>
struct packed_monomial
{
    u64 words[W] = {};

    [[nodiscard]] friend constexpr bool operator==(packed_monomial const& a, packed_monomial const& b)
    {
        for (auto i = 0; i < W; ++i)
            if (a.words[i] != b.words[i])
                return false;
        return true;
    }
    [[nodiscard]] friend constexpr packed_monomial operator+(packed_monomial const& a, packed_monomial const& b)
    {
        packed_monomial r;
        for (auto i = 0; i < W; ++i)
            r.words[i] = a.words[i] + b.words[i];
        return r;
    }
    [[nodiscard]] friend constexpr packed_monomial operator-(packed_monomial const& a, packed_monomial const& b)
    {
        packed_monomial r;
        for (auto i = 0; i < W; ++i)
            r.words[i] = a.words[i] - b.words[i];
        return r;
    }
    [[nodiscard]] constexpr bool intersects(packed_monomial const& mask) const
    {
        u64 any = 0;
        for (auto i = 0; i < W; ++i)
            any |= words[i] & mask.words[i];
        return any != 0;
    }
};

template <class P, class C>
inline constexpr bool is_multi_polynomial_over = false;
template <class C, class Vars>
inline constexpr bool is_multi_polynomial_over<tg::multi_polynomial<C, Vars>, C> = true;

template <class S, class C>
concept multi_polynomial_scalar_of = std::is_convertible_v<S const&, C> && !is_multi_polynomial_over<S, C>;

/// -1, 0 or +1: the coefficient's own sign() where it has one (a bigint, a nested infinitesimal polynomial), its comparison with 0 otherwise.
template <class C>
[[nodiscard]] int coefficient_sign(C const& c)
{
    if constexpr (requires { c.sign(); })
        return c.sign();
    else
        return int(C(0) < c) - int(c < C(0));
}
} // namespace tg::impl

/// N statically indexed variables, each exponent a Bits-wide field whose top bit guards against overflow:
/// an exponent, and under a graded order the total degree, is at most 2^(Bits - 1) - 1, checked in every build.
/// Bits 16 packs four fields into a word: four variables, or three under a graded order, compare as one u64.
template <int N, class Order, int Bits>
struct tg::vars
{
    static_assert(N >= 1, "tg::vars: at least one variable");
    static_assert(Bits == 8 || Bits == 16 || Bits == 32, "tg::vars: an exponent field is 8, 16 or 32 bits wide");

    using order = Order;
    static constexpr int count = N;
    static constexpr int bits = Bits;
    static constexpr int fields = N + (Order::graded ? 1 : 0);
    static constexpr int fields_per_word = 64 / Bits;
    static constexpr int words = (fields + fields_per_word - 1) / fields_per_word;
    static constexpr i64 max_exponent = (i64(1) << (Bits - 1)) - 1;

    using key = impl::packed_monomial<words>;
    using exponents = cc::fixed_array<int, N>;

    /// The field holding variable v's exponent, field 0 the most significant.
    [[nodiscard]] static constexpr int field_of(int v)
    {
        return (Order::graded ? 1 : 0) + (Order::variables_reversed ? N - 1 - v : v);
    }
    [[nodiscard]] static constexpr int shift_of(int field) { return 64 - Bits * (field % fields_per_word + 1); }

    [[nodiscard]] static constexpr key field_mask(int first_field, u64 per_field)
    {
        key k;
        for (auto f = first_field; f < fields; ++f)
            k.words[f / fields_per_word] |= per_field << shift_of(f);
        return k;
    }
    static constexpr key guard_mask = field_mask(0, u64(1) << (Bits - 1));
    /// XORed into both sides before comparing: the variable fields of an order that compares them reversed.
    static constexpr key compare_mask
        = Order::compare_reversed ? field_mask(Order::graded ? 1 : 0, (u64(1) << Bits) - 1) : key{};

    [[nodiscard]] static key pack(exponents const& e)
    {
        key k;
        i64 degree = 0;
        for (auto v = 0; v < N; ++v)
        {
            CC_ASSERT_ALWAYS(0 <= e[v] && e[v] <= max_exponent, "tg::vars: an exponent past the field width");
            auto const f = field_of(v);
            k.words[f / fields_per_word] |= u64(e[v]) << shift_of(f);
            degree += e[v];
        }
        if constexpr (Order::graded)
        {
            CC_ASSERT_ALWAYS(degree <= max_exponent, "tg::vars: a total degree past the field width");
            k.words[0] |= u64(degree) << shift_of(0);
        }
        return k;
    }

    [[nodiscard]] static int exponent(key const& k, int v)
    {
        auto const f = field_of(v);
        return int((k.words[f / fields_per_word] >> shift_of(f)) & ((u64(1) << Bits) - 1));
    }

    [[nodiscard]] static exponents unpack(key const& k)
    {
        exponents e;
        for (auto v = 0; v < N; ++v)
            e[v] = exponent(k, v);
        return e;
    }

    /// The monomial a * b, checked against overflow.
    [[nodiscard]] static key multiply(key const& a, key const& b)
    {
        auto const r = a + b;
        CC_ASSERT_ALWAYS(!r.intersects(guard_mask), "tg::vars: a product's exponent past the field width");
        return r;
    }

    /// Whether a ranks above b under the order.
    [[nodiscard]] static constexpr bool greater(key const& a, key const& b)
    {
        for (auto i = 0; i < words; ++i)
        {
            auto const x = a.words[i] ^ compare_mask.words[i];
            auto const y = b.words[i] ^ compare_mask.words[i];
            if (x != y)
                return x > y;
        }
        return false;
    }
};

/// A sum of terms c * x0^e0 * ... * x(N-1)^e(N-1) over a coefficient type C, by duck typing as tg::polynomial.
/// The terms are sorted by Vars' order, leading term first, with no zero coefficient and no repeated monomial;
/// exponents and coefficients sit in two parallel arrays, the exponents packed (see tg::vars).
/// Over a ring it is a ring, and a multi_polynomial is itself a valid C: one over another's variables nests,
/// as a polynomial in infinitesimals whose coefficients are polynomials in the inputs.
template <class C, class Vars>
struct tg::multi_polynomial
{
    using coefficient_t = C;
    using vars_t = Vars;
    using key_t = typename Vars::key;
    using exponents_t = typename Vars::exponents;
    static constexpr int variable_count = Vars::count;

    multi_polynomial() = default;

    /// The constant c.
    template <class S>
        requires impl::multi_polynomial_scalar_of<S, C>
    multi_polynomial(S const& c)
    {
        C v(c);
        if (!impl::coefficient_is_zero(v))
            _push(key_t{}, cc::move(v));
    }

    /// c * x0^e[0] * ... * x(N-1)^e[N-1].
    [[nodiscard]] static multi_polynomial monomial(C c, exponents_t const& e)
    {
        multi_polynomial p;
        auto const k = Vars::pack(e);
        if (!impl::coefficient_is_zero(c))
            p._push(k, cc::move(c));
        return p;
    }

    /// The variable x(v).
    [[nodiscard]] static multi_polynomial variable(int v)
    {
        CC_ASSERT(0 <= v && v < variable_count, "tg::multi_polynomial::variable: no such variable");
        exponents_t e = {};
        e[v] = 1;
        return monomial(C(1), e);
    }

    /// x0, ..., x(N-1).
    [[nodiscard]] static cc::fixed_array<multi_polynomial, Vars::count> generators()
    {
        cc::fixed_array<multi_polynomial, Vars::count> g;
        for (auto v = 0; v < variable_count; ++v)
            g[v] = variable(v);
        return g;
    }

    // observers

    [[nodiscard]] bool is_zero() const { return _coeffs.empty(); }
    [[nodiscard]] bool is_constant() const { return _coeffs.empty() || (_coeffs.size() == 1 && _keys[0] == key_t{}); }
    [[nodiscard]] int term_count() const { return int(_coeffs.size()); }

    /// Term t's coefficient and exponents, t = 0 the leading term.
    [[nodiscard]] C const& coefficient(int t) const { return _coeffs[t]; }
    [[nodiscard]] exponents_t exponents(int t) const { return Vars::unpack(_keys[t]); }
    [[nodiscard]] int exponent(int t, int v) const { return Vars::exponent(_keys[t], v); }

    /// The coefficient of the monomial with exponents e, zero when there is no such term.
    [[nodiscard]] C coefficient_of(exponents_t const& e) const
    {
        auto const t = _find(Vars::pack(e));
        return t >= 0 ? _coeffs[t] : C(0);
    }

    /// The leading term's coefficient; the polynomial must not be zero.
    [[nodiscard]] C const& leading_coefficient() const
    {
        CC_ASSERT(!is_zero(), "tg::multi_polynomial: the zero polynomial has no leading term");
        return _coeffs[0];
    }
    [[nodiscard]] exponents_t leading_exponents() const
    {
        CC_ASSERT(!is_zero(), "tg::multi_polynomial: the zero polynomial has no leading term");
        return Vars::unpack(_keys[0]);
    }

    /// The highest exponent of x(v), -1 for the zero polynomial.
    [[nodiscard]] int degree(int v) const
    {
        auto d = is_zero() ? -1 : 0;
        for (auto const& k : _keys)
            d = cc::max(d, Vars::exponent(k, v));
        return d;
    }
    /// The highest total degree of a term, -1 for the zero polynomial.
    [[nodiscard]] int total_degree() const
    {
        auto d = is_zero() ? -1 : 0;
        for (auto const& k : _keys)
        {
            auto s = 0;
            for (auto v = 0; v < variable_count; ++v)
                s += Vars::exponent(k, v);
            d = cc::max(d, s);
        }
        return d;
    }

    /// The sign for infinitesimal variables, positive but smaller than any positive value: the leading coefficient's.
    [[nodiscard]] int sign() const
        requires std::is_same_v<typename Vars::order, infinitesimal_order>
    {
        return is_zero() ? 0 : impl::coefficient_sign(_coeffs[0]);
    }

    /// The value at x[0], ..., x[N-1] in C, so a braced list of values works: p({2, 3}).
    [[nodiscard]] C operator()(cc::fixed_array<C, Vars::count> const& x) const
    {
        return this->template operator()<C>(x);
    }

    /// The value at x[0], ..., x[N-1], in X where X is built from C, in C otherwise.
    template <class X>
    [[nodiscard]] auto operator()(cc::fixed_array<X, Vars::count> const& x) const
    {
        using R = std::conditional_t<std::is_constructible_v<X, C const&>, X, C>;
        // powers[v][e] = x[v]^e up to the highest exponent of x(v)
        cc::fixed_array<cc::vector<R>, Vars::count> powers;
        for (auto v = 0; v < variable_count; ++v)
        {
            auto const d = degree(v);
            powers[v].push_back(R(1));
            for (auto e = 1; e <= d; ++e)
                powers[v].push_back(powers[v].back() * R(x[v]));
        }
        auto r = R(0);
        for (auto t = 0; t < term_count(); ++t)
        {
            auto term = R(_coeffs[t]);
            for (auto v = 0; v < variable_count; ++v)
                if (auto const e = Vars::exponent(_keys[t], v); e > 0)
                    term = term * powers[v][e];
            r += term;
        }
        return r;
    }

    /// The terms in order, as "3*x0^2*x1 - x1 + 1"; names default to x0, x1, ...
    [[nodiscard]] cc::string to_string(cc::span<cc::string_view const> names = {}) const;

    // arithmetic

    void negate()
    {
        for (auto& c : _coeffs)
            c = -c;
    }

    [[nodiscard]] friend multi_polynomial operator-(multi_polynomial a)
    {
        a.negate();
        return a;
    }
    [[nodiscard]] friend multi_polynomial operator+(multi_polynomial const& a, multi_polynomial const& b)
    {
        return _merge(a, b, false);
    }
    [[nodiscard]] friend multi_polynomial operator-(multi_polynomial const& a, multi_polynomial const& b)
    {
        return _merge(a, b, true);
    }
    [[nodiscard]] friend multi_polynomial operator*(multi_polynomial const& a, multi_polynomial const& b)
    {
        return _product(a, b);
    }
    // an rvalue operand's storage becomes the result's, its coefficients moved rather than copied
    [[nodiscard]] friend multi_polynomial operator+(multi_polynomial&& a, multi_polynomial const& b)
    {
        a._merge_into(b, false);
        return cc::move(a);
    }
    [[nodiscard]] friend multi_polynomial operator+(multi_polynomial const& a, multi_polynomial&& b)
    {
        b._merge_into(a, false);
        return cc::move(b);
    }
    [[nodiscard]] friend multi_polynomial operator+(multi_polynomial&& a, multi_polynomial&& b)
    {
        a._merge_into(b, false);
        return cc::move(a);
    }
    [[nodiscard]] friend multi_polynomial operator-(multi_polynomial&& a, multi_polynomial const& b)
    {
        a._merge_into(b, true);
        return cc::move(a);
    }
    [[nodiscard]] friend multi_polynomial operator-(multi_polynomial const& a, multi_polynomial&& b)
    {
        b.negate();
        b._merge_into(a, false);
        return cc::move(b);
    }
    [[nodiscard]] friend multi_polynomial operator-(multi_polynomial&& a, multi_polynomial&& b)
    {
        a._merge_into(b, true);
        return cc::move(a);
    }
    multi_polynomial& operator+=(multi_polynomial const& b)
    {
        _merge_into(b, false);
        return *this;
    }
    multi_polynomial& operator-=(multi_polynomial const& b)
    {
        _merge_into(b, true);
        return *this;
    }
    multi_polynomial& operator*=(multi_polynomial const& b) { return *this = _product(*this, b); }

    // a scalar operand takes these over a conversion to a polynomial
    template <class S>
        requires impl::multi_polynomial_scalar_of<S, C>
    [[nodiscard]] friend multi_polynomial operator*(multi_polynomial a, S const& s)
    {
        a._scale(C(s));
        return a;
    }
    template <class S>
        requires impl::multi_polynomial_scalar_of<S, C>
    [[nodiscard]] friend multi_polynomial operator*(S const& s, multi_polynomial a)
    {
        a._scale(C(s));
        return a;
    }
    template <class S>
        requires impl::multi_polynomial_scalar_of<S, C>
    multi_polynomial& operator*=(S const& s)
    {
        _scale(C(s));
        return *this;
    }

    [[nodiscard]] friend bool operator==(multi_polynomial const& a, multi_polynomial const& b)
    {
        if (a.term_count() != b.term_count())
            return false;
        for (auto t = 0; t < a.term_count(); ++t)
            if (!(a._keys[t] == b._keys[t]) || !(a._coeffs[t] == b._coeffs[t]))
                return false;
        return true;
    }

    /// d/dx(v); a term's order survives division by x(v), so no sort.
    [[nodiscard]] friend multi_polynomial derivative(multi_polynomial const& p, int v)
    {
        exponents_t unit = {};
        unit[v] = 1;
        auto const step = Vars::pack(unit);
        multi_polynomial r;
        for (auto t = 0; t < p.term_count(); ++t)
            if (auto const e = Vars::exponent(p._keys[t], v); e > 0)
            {
                auto c = p._coeffs[t] * C(e);
                if (!impl::coefficient_is_zero(c))
                    r._push(p._keys[t] - step, cc::move(c));
            }
        return r;
    }

    /// p with x(v) = value: a polynomial no longer in x(v).
    [[nodiscard]] friend multi_polynomial substitute(multi_polynomial const& p, int v, C const& value)
    {
        auto const powers = _powers_of(value, p.degree(v));
        cc::vector<key_t> keys;
        cc::vector<C> coeffs;
        for (auto t = 0; t < p.term_count(); ++t)
        {
            auto e = Vars::unpack(p._keys[t]);
            auto const d = e[v];
            e[v] = 0;
            keys.push_back(Vars::pack(e));
            coeffs.push_back(p._coeffs[t] * powers[d]);
        }
        return _from_unsorted(cc::move(keys), cc::move(coeffs));
    }

    /// p as a polynomial in x(v) whose coefficients are polynomials in the other variables.
    [[nodiscard]] friend polynomial<multi_polynomial> to_univariate(multi_polynomial const& p, int v)
    {
        auto const d = p.degree(v);
        if (d < 0)
            return {};
        cc::vector<cc::vector<key_t>> keys;
        cc::vector<cc::vector<C>> coeffs;
        keys.resize_to_defaulted(d + 1);
        coeffs.resize_to_defaulted(d + 1);
        for (auto t = 0; t < p.term_count(); ++t)
        {
            auto e = Vars::unpack(p._keys[t]);
            auto const k = e[v];
            e[v] = 0;
            keys[k].push_back(Vars::pack(e));
            coeffs[k].push_back(p._coeffs[t]);
        }
        auto u = polynomial<multi_polynomial>::zeroed(d + 1);
        for (auto k = 0; k <= d; ++k)
            u[k] = _from_unsorted(cc::move(keys[k]), cc::move(coeffs[k]));
        u.trim();
        return u;
    }

    /// a / b when b divides a, empty otherwise; b must not be zero.
    /// Leading terms divide in turn, which holds under any order compatible with products, the local infinitesimal one included,
    /// since the remainder's leading term is the next quotient term's times b's.
    /// A remainder past a's degree in some variable cannot come from an exact quotient, which ends a division that is not one:
    /// under a local order it would otherwise run on forever, 1 / (1 - x) being a series.
    [[nodiscard]] friend cc::optional<multi_polynomial> try_div_exact(multi_polynomial const& a, multi_polynomial const& b)
    {
        CC_ASSERT(!b.is_zero(), "tg::multi_polynomial: division by the zero polynomial");
        multi_polynomial q;
        if (a.is_zero())
            return q;
        exponents_t bound;
        for (auto v = 0; v < variable_count; ++v)
            bound[v] = a.degree(v);
        auto const lb = Vars::unpack(b._keys[0]);
        auto const& lc = b._coeffs[0];
        auto r = a;
        while (!r.is_zero())
        {
            auto const lr = Vars::unpack(r._keys[0]);
            exponents_t m;
            for (auto v = 0; v < variable_count; ++v)
            {
                if (lr[v] > bound[v] || lr[v] < lb[v])
                    return {};
                m[v] = lr[v] - lb[v];
            }
            C c;
            if (!impl::try_exact_quotient(r._coeffs[0], lc, c))
                return {};
            auto const t = monomial(c, m);
            r -= t * b;
            q._push(t._keys[0], cc::move(c));
        }
        return q;
    }

    /// a / b where b divides a.
    [[nodiscard]] friend multi_polynomial div_exact(multi_polynomial const& a, multi_polynomial const& b)
    {
        auto q = try_div_exact(a, b);
        CC_ASSERT_ALWAYS(q.has_value(), "tg::div_exact: the divisor does not divide");
        return cc::move(q.value());
    }

    /// The greatest common divisor, its leading coefficient positive, over coefficients with a gcd of their own or an exact field.
    /// Recursive: a and b as polynomials in one variable over the others, the subresultant gcd there,
    /// whose coefficients' gcds recurse over the remaining variables down to the coefficients' own.
    [[nodiscard]] friend multi_polynomial gcd(multi_polynomial const& a, multi_polynomial const& b)
        requires(impl::has_gcd<C> || (traits::is_field<C> && traits::is_exact<C>))
    {
        auto const normalized = [](multi_polynomial p)
        {
            if (!p.is_zero() && impl::leading_sign(p._coeffs[0]) < 0)
                p.negate();
            return p;
        };
        if (a.is_zero())
            return normalized(b);
        if (b.is_zero())
            return normalized(a);
        auto v = -1;
        for (auto w = 0; w < variable_count && v < 0; ++w)
            if (a.degree(w) > 0 || b.degree(w) > 0)
                v = w;
        if (v < 0)
        {
            // two constants
            if constexpr (impl::has_gcd<C>)
                return normalized(multi_polynomial(gcd(a._coeffs[0], b._coeffs[0])));
            else
                return multi_polynomial(C(1));
        }
        if constexpr (std::is_same_v<C, bigint>)
            if (auto h = _heuristic_gcd(a, b); h.has_value())
                return normalized(h.value());
        return normalized(from_univariate(gcd(to_univariate(a, v), to_univariate(b, v)), v));
    }

    /// The inverse of to_univariate: u's coefficients times powers of x(v).
    [[nodiscard]] friend multi_polynomial from_univariate(polynomial<multi_polynomial> const& u, int v)
    {
        multi_polynomial r;
        auto const x = variable(v);
        for (auto k = u.degree(); k >= 0; --k)
            r = r * x + u[k];
        return r;
    }

private:
    /// The gcd of the coefficients, positive.
    [[nodiscard]] C _integer_content() const
    {
        auto g = C(0);
        for (auto const& c : _coeffs)
            g = gcd(g, c);
        return g;
    }

    /// Every coefficient divided by d, which divides each of them.
    [[nodiscard]] multi_polynomial _divided_by(C const& d) const
    {
        multi_polynomial r = *this;
        if (!traits::is_one(d))
            for (auto& c : r._coeffs)
                c = div_exact(c, d);
        return r;
    }

    /// The largest coefficient's magnitude.
    [[nodiscard]] C _max_norm() const
    {
        auto m = C(0);
        for (auto const& c : _coeffs)
            if (auto const x = abs(c); x > m)
                m = x;
        return m;
    }

    /// The heuristic gcd of Char, Geddes and Gonnet over the integers, or empty where it fails.
    /// The integer contents are taken apart at every level, the method holding for primitive inputs:
    /// the evaluations one level down are not primitive, and their common integer factor is what this level reads back.
    /// The first present variable is evaluated at an integer xi above twice the smaller input's largest coefficient,
    /// the evaluations' gcd taken by the same method one variable down, an integer gcd at the bottom,
    /// and a candidate read back from that gcd's balanced base-xi digits; division of both inputs proves it.
    /// A failed candidate grows xi, and a few failures hand the gcd to the subresultant sequence.
    [[nodiscard]] static cc::optional<multi_polynomial> _heuristic_gcd(multi_polynomial const& a,
                                                                       multi_polynomial const& b)
    {
        auto v = -1;
        for (auto w = 0; w < variable_count && v < 0; ++w)
            if (a.degree(w) > 0 || b.degree(w) > 0)
                v = w;
        if (v < 0)
            return multi_polynomial(gcd(a.is_zero() ? C(0) : a._coeffs[0], b.is_zero() ? C(0) : b._coeffs[0]));
        if (a.is_zero() || b.is_zero())
            return a.is_zero() ? b : a;

        auto const content = gcd(a._integer_content(), b._integer_content());
        auto const pa = a._divided_by(a._integer_content());
        auto const pb = b._divided_by(b._integer_content());
        auto const na = pa._max_norm();
        auto const nb = pb._max_norm();
        auto xi = (na < nb ? na : nb) * 2 + 29;
        for (auto attempt = 0; attempt < 6; ++attempt)
        {
            auto const gamma = _heuristic_gcd(substitute(pa, v, xi), substitute(pb, v, xi));
            if (gamma.has_value())
            {
                // the balanced base-xi digits of gamma are the candidate's coefficients in x(v)
                auto rest = gamma.value();
                cc::vector<multi_polynomial> digits;
                auto const half = xi / 2;
                while (!rest.is_zero())
                {
                    multi_polynomial digit;
                    for (auto t = 0; t < rest.term_count(); ++t)
                    {
                        auto c = rest._coeffs[t] % xi;
                        if (c > half)
                            c = c - xi;
                        else if (c < -half)
                            c = c + xi;
                        if (!c.is_zero())
                            digit._push(rest._keys[t], cc::move(c));
                    }
                    rest = (rest - digit)._divided_by(xi);
                    digits.push_back(cc::move(digit));
                }
                auto u = polynomial<multi_polynomial>::zeroed(int(digits.size()));
                for (isize i = 0; i < digits.size(); ++i)
                    u[int(i)] = cc::move(digits[i]);
                u.trim();
                auto candidate = from_univariate(u, v);
                if (!candidate.is_zero())
                {
                    candidate = candidate._divided_by(candidate._integer_content());
                    if (try_div_exact(pa, candidate).has_value() && try_div_exact(pb, candidate).has_value())
                        return candidate * content;
                }
            }
            // xi grows by a factor near 2.73 that avoids small rational ratios, as the method's authors chose
            xi = xi * 73794 / 27011;
        }
        return {};
    }

    void _push(key_t const& k, C c)
    {
        _keys.push_back(k);
        _coeffs.push_back(cc::move(c));
    }

    void _scale(C const& s)
    {
        if (impl::coefficient_is_zero(s))
        {
            _keys.clear();
            _coeffs.clear();
            return;
        }
        // a ring with zero divisors (a wrapping fixed_int) may cancel a term
        auto n = 0;
        for (auto t = 0; t < term_count(); ++t)
        {
            auto c = _coeffs[t] * s;
            if (impl::coefficient_is_zero(c))
                continue;
            _keys[n] = _keys[t];
            _coeffs[n] = cc::move(c);
            ++n;
        }
        _keys.resize_down_to(n);
        _coeffs.resize_down_to(n);
    }

    [[nodiscard]] int _find(key_t const& k) const
    {
        auto lo = 0;
        auto hi = term_count();
        while (lo < hi)
        {
            auto const mid = (lo + hi) / 2;
            if (Vars::greater(_keys[mid], k))
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo < term_count() && _keys[lo] == k ? lo : -1;
    }

    [[nodiscard]] static cc::vector<C> _powers_of(C const& x, int d)
    {
        cc::vector<C> p;
        p.push_back(C(1));
        for (auto e = 1; e <= d; ++e)
            p.push_back(p.back() * x);
        return p;
    }

    /// The sum or difference by merging the two sorted term lists.
    [[nodiscard]] static multi_polynomial _merge(multi_polynomial const& a, multi_polynomial const& b, bool subtract)
    {
        multi_polynomial r;
        r._keys.reserve(a.term_count() + b.term_count());
        r._coeffs.reserve(a.term_count() + b.term_count());
        auto i = 0;
        auto j = 0;
        while (i < a.term_count() && j < b.term_count())
        {
            if (Vars::greater(a._keys[i], b._keys[j]))
            {
                r._push(a._keys[i], a._coeffs[i]);
                ++i;
            }
            else if (Vars::greater(b._keys[j], a._keys[i]))
            {
                r._push(b._keys[j], subtract ? -b._coeffs[j] : b._coeffs[j]);
                ++j;
            }
            else
            {
                auto c = subtract ? a._coeffs[i] - b._coeffs[j] : a._coeffs[i] + b._coeffs[j];
                if (!impl::coefficient_is_zero(c))
                    r._push(a._keys[i], cc::move(c));
                ++i;
                ++j;
            }
        }
        for (; i < a.term_count(); ++i)
            r._push(a._keys[i], a._coeffs[i]);
        for (; j < b.term_count(); ++j)
            r._push(b._keys[j], subtract ? -b._coeffs[j] : b._coeffs[j]);
        return r;
    }

    /// *this ± b in place: grown by b's terms and merged from the back, the smallest terms last,
    /// then compacted over any terms that cancelled; b may be *this.
    void _merge_into(multi_polynomial const& b, bool subtract)
    {
        if (&b == this)
        {
            if (subtract)
                *this = {};
            else
                _scale(C(2));
            return;
        }
        auto const na = term_count();
        auto const nb = b.term_count();
        if (nb == 0)
            return;
        _keys.resize_to_defaulted(na + nb);
        _coeffs.resize_to_filled(na + nb, C(0));
        auto i = na - 1;
        auto j = nb - 1;
        auto w = na + nb - 1;
        while (j >= 0)
        {
            if (i >= 0 && Vars::greater(b._keys[j], _keys[i]))
            {
                _keys[w] = _keys[i];
                _coeffs[w] = cc::move(_coeffs[i]);
                --i;
            }
            else if (i >= 0 && _keys[i] == b._keys[j])
            {
                _keys[w] = _keys[i];
                _coeffs[w] = cc::move(_coeffs[i]);
                if (subtract)
                    _coeffs[w] -= b._coeffs[j];
                else
                    _coeffs[w] += b._coeffs[j];
                --i;
                --j;
            }
            else
            {
                _keys[w] = b._keys[j];
                _coeffs[w] = subtract ? -b._coeffs[j] : b._coeffs[j];
                --j;
            }
            --w;
        }
        // [0, i] never moved; (w, na + nb) holds the rest, with a gap between where terms combined, and maybe zeros
        auto dst = i + 1;
        for (auto src = w + 1; src < na + nb; ++src)
        {
            if (impl::coefficient_is_zero(_coeffs[src]))
                continue;
            if (dst != src)
            {
                _keys[dst] = _keys[src];
                _coeffs[dst] = cc::move(_coeffs[src]);
            }
            ++dst;
        }
        _keys.resize_down_to(dst);
        _coeffs.resize_down_to(dst);
    }

    /// The product, densely when the exponents' box is small and by a heap otherwise, each coefficient accumulated in place.
    /// A single-term factor keeps the other's order, the order being compatible with multiplication, so it needs neither.
    [[nodiscard]] static multi_polynomial _product(multi_polynomial const& a, multi_polynomial const& b)
    {
        multi_polynomial r;
        if (a.is_zero() || b.is_zero())
            return r;
        if (a.term_count() == 1 || b.term_count() == 1)
        {
            auto const& single = a.term_count() == 1 ? a : b;
            auto const& other = a.term_count() == 1 ? b : a;
            for (auto t = 0; t < other.term_count(); ++t)
            {
                auto c
                    = a.term_count() == 1 ? single._coeffs[0] * other._coeffs[t] : other._coeffs[t] * single._coeffs[0];
                if (!impl::coefficient_is_zero(c))
                    r._push(Vars::multiply(single._keys[0], other._keys[t]), cc::move(c));
            }
            return r;
        }

        // each coefficient summed in one slot: a word accumulator where every factor is small enough, a C otherwise
        return impl::with_slot<C>(impl::widest_limbs<C>(a._coeffs, b._coeffs),
                                  [&]<class S>() -> multi_polynomial
                                  {
                                      multi_polynomial p;
                                      if (_product_dense<S>(a, b, p))
                                          return p;
                                      return _product_heap<S>(a, b);
                                  });
    }

    /// The product by Kronecker substitution, when the box of its exponents is small against the pairs it sums:
    /// each term's exponents become one index into a dense accumulator, a pair's product lands at the sum of its factors' indices,
    /// and the slots it touched are read out in the order.
    /// False, with r untouched, when the box is too large or an exponent would overflow.
    template <class S>
    [[nodiscard]] static bool _product_dense(multi_polynomial const& a, multi_polynomial const& b, multi_polynomial& r)
    {
        constexpr auto n = variable_count;
        exponents_t da = {};
        exponents_t db = {};
        for (auto const& k : a._keys)
            for (auto v = 0; v < n; ++v)
                da[v] = cc::max(da[v], Vars::exponent(k, v));
        for (auto const& k : b._keys)
            for (auto v = 0; v < n; ++v)
                db[v] = cc::max(db[v], Vars::exponent(k, v));

        // a box up to four slots per pair, under 2^22 slots, stays in cache against the pairs that write it
        auto const pairs = i64(a.term_count()) * b.term_count();
        auto const limit = cc::min(4 * pairs + 16, i64(1) << 22);
        cc::fixed_array<i64, Vars::count> stride;
        cc::fixed_array<int, Vars::count> extent;
        i64 size = 1;
        for (auto q = 0; q < n; ++q)
        {
            // the least significant variable first, x0 under a reversed layout and x(N-1) otherwise, so index order is the order's
            auto const v = Vars::order::variables_reversed ? q : n - 1 - q;
            if (da[v] + db[v] > Vars::max_exponent)
                return false;
            stride[v] = size;
            extent[v] = da[v] + db[v] + 1;
            size *= extent[v];
            if (size > limit)
                return false;
        }
        if constexpr (Vars::order::graded)
            if (a.total_degree() + b.total_degree() > Vars::max_exponent)
                return false;

        // the accumulator is all zeros between calls: every slot read out is reset
        thread_local cc::vector<S> dense;
        thread_local cc::vector<u8> seen;
        thread_local cc::vector<i64> index_a;
        thread_local cc::vector<i64> index_b;
        if (dense.size() < size)
        {
            dense.resize_to_filled(size, impl::zero_slot<C, S>());
            seen.resize_to_filled(size, u8(0));
        }
        auto const index_of = [&](key_t const& k)
        {
            i64 i = 0;
            for (auto v = 0; v < n; ++v)
                i += Vars::exponent(k, v) * stride[v];
            return i;
        };
        index_a.resize_to_uninitialized(a.term_count());
        index_b.resize_to_uninitialized(b.term_count());
        for (auto t = 0; t < a.term_count(); ++t)
            index_a[t] = index_of(a._keys[t]);
        for (auto t = 0; t < b.term_count(); ++t)
            index_b[t] = index_of(b._keys[t]);

        auto const fa = impl::slot_factors<C, S>(a._coeffs);
        auto const fb = impl::slot_factors<C, S>(b._coeffs);
        // raw pointers and counts in locals: the slots' stores could alias the vectors' headers, which would be reloaded every pair
        auto const nb = b.term_count();
        auto const* const ib = index_b.data();
        auto const* const fbp = fb.data();
        auto const* const kb = b._keys.data();
        auto* const dense_base = dense.data();
        auto* const seen_base = seen.data();

        if (size <= pairs)
        {
            // A box no larger than the pairs: accumulate untracked and walk every slot once, a slab of the leading variable at a time.
            // Output slab k holds the pairs whose leading exponents sum to k, so only a slab's accumulators are live at once,
            // which keeps them in cache where the whole box would not fit.
            // The walk is an odometer, least significant variable fastest, carrying the packed key along:
            // a key is linear in the exponents, so a step adds a variable's unit and a carry takes its wrap back off.
            // A larger index is a greater monomial unless the order compares its fields reversed, which walks upwards;
            // a graded order sorts the result afterwards.
            constexpr auto lead = Vars::order::variables_reversed ? n - 1 : 0;
            auto const slab = stride[lead];

            // the terms counting-sorted by their leading exponent, their factors and their indices less the leading part
            struct sorted_terms
            {
                cc::vector<int> start; // bucket e is [start[e], start[e + 1])
                cc::vector<i64> rest;
                cc::vector<typename impl::slot_factor<C, S>::type> factor;
            };
            auto const sort_terms
                = [&](multi_polynomial const& p, cc::vector<i64> const& index, auto const& factors, int top)
            {
                sorted_terms st;
                st.start.resize_to_filled(top + 2, 0);
                for (auto const& k : p._keys)
                    ++st.start[Vars::exponent(k, lead) + 1];
                for (auto e = 0; e <= top; ++e)
                    st.start[e + 1] += st.start[e];
                auto fill = st.start;
                st.rest.resize_to_filled(p.term_count(), i64(0));
                st.factor.resize_to_filled(p.term_count(), factors[0]);
                for (auto t = 0; t < p.term_count(); ++t)
                {
                    auto const e = Vars::exponent(p._keys[t], lead);
                    auto const at = fill[e]++;
                    st.rest[at] = index[t] - e * slab;
                    st.factor[at] = factors[t];
                }
                return st;
            };
            auto const sa = sort_terms(a, index_a, fa, da[lead]);
            auto const sb = sort_terms(b, index_b, fb, db[lead]);

            constexpr auto upwards = Vars::order::compare_reversed;
            cc::fixed_array<key_t, Vars::count> unit;
            cc::fixed_array<key_t, Vars::count> wrap;
            for (auto v = 0; v < n; ++v)
            {
                exponents_t u = {};
                u[v] = 1;
                unit[v] = Vars::pack(u);
                u[v] = extent[v] - 1;
                wrap[v] = Vars::pack(u);
            }
            // multiple[v][x] = the key of x in variable v alone, for taking an exponent back off in one step
            cc::fixed_array<cc::vector<key_t>, Vars::count> multiple;
            for (auto v = 0; v < n; ++v)
                for (auto x = 0; x < extent[v]; ++x)
                {
                    exponents_t u = {};
                    u[v] = x;
                    multiple[v].push_back(Vars::pack(u));
                }
            auto const max_total = a.total_degree() + b.total_degree();
            auto const top = extent[lead] - 1;
            r._keys.reserve(cc::min(size, pairs));
            r._coeffs.reserve(cc::min(size, pairs));
            for (auto step = 0; step <= top; ++step)
            {
                auto const k = upwards ? step : top - step;
                auto const lo = cc::max(0, k - db[lead]);
                auto const hi = cc::min(k, da[lead]);
                for (auto ea = lo; ea <= hi; ++ea)
                {
                    auto const eb = k - ea;
                    auto const b_begin = sb.start[eb];
                    auto const b_end = sb.start[eb + 1];
                    auto const* const rb = sb.rest.data();
                    auto const* const fbs = sb.factor.data();
                    for (auto ii = sa.start[ea]; ii < sa.start[ea + 1]; ++ii)
                    {
                        auto* const row = dense_base + sa.rest[ii];
                        auto const ca = sa.factor[ii];
                        for (auto jj = b_begin; jj < b_end; ++jj)
                            impl::slot_add_mul_factors<C>(row[rb[jj]], ca, fbs[jj]);
                    }
                }

                // read the slab out, the leading exponent held at k
                if constexpr (upwards)
                {
                    // Upwards, the odometer skips past the product's total degree: a slot beyond it was never written,
                    // and a simplex-shaped support such as Fateman's fills a few percent of its box.
                    exponents_t e = {};
                    e[lead] = k;
                    auto key = Vars::pack(e);
                    i64 slot = 0;
                    auto sum = k;
                    while (true)
                    {
                        if (!impl::slot_is_zero<C>(dense_base[slot]))
                            r._push(key, impl::slot_take<C>(dense_base[slot]));
                        auto advanced = false;
                        for (auto q = 0; q < n; ++q)
                        {
                            auto const v = Vars::order::variables_reversed ? q : n - 1 - q;
                            if (v == lead)
                                break;
                            if (sum < max_total && e[v] + 1 < extent[v])
                            {
                                ++e[v];
                                ++sum;
                                key = key + unit[v];
                                slot += stride[v];
                                advanced = true;
                                break;
                            }
                            sum -= e[v];
                            slot -= e[v] * stride[v];
                            key = key - multiple[v][e[v]];
                            e[v] = 0;
                        }
                        if (!advanced)
                            break;
                    }
                }
                else
                {
                    exponents_t e = {};
                    for (auto v = 0; v < n; ++v)
                        if (v != lead)
                            e[v] = extent[v] - 1;
                    e[lead] = k;
                    auto key = Vars::pack(e);
                    auto slot = slab - 1;
                    for (i64 left = slab; left > 0; --left)
                    {
                        if (!impl::slot_is_zero<C>(dense_base[slot]))
                            r._push(key, impl::slot_take<C>(dense_base[slot]));
                        --slot;
                        if (left == 1)
                            break;
                        for (auto q = 0; q < n; ++q)
                        {
                            auto const v = Vars::order::variables_reversed ? q : n - 1 - q;
                            if (e[v] > 0)
                            {
                                --e[v];
                                key = key - unit[v];
                                break;
                            }
                            e[v] = extent[v] - 1;
                            key = key + wrap[v];
                        }
                    }
                }
            }
            if constexpr (Vars::order::graded)
                r._sort_terms();
            return true;
        }

        // A sparser box: a slot's first product records its monomial, marked in seen until the read-out clears it.
        struct touched_slot
        {
            key_t key;
            i64 slot;
        };
        thread_local cc::vector<touched_slot> touched;
        touched.clear();
        for (auto i = 0; i < a.term_count(); ++i)
        {
            auto const ia = index_a[i];
            auto* const row = dense_base + ia;
            auto* const row_seen = seen_base + ia;
            auto const ca = fa[i];
            auto const ka = a._keys[i];
            for (auto j = 0; j < nb; ++j)
            {
                auto const at = ib[j];
                if (!row_seen[at]) [[unlikely]]
                {
                    row_seen[at] = 1;
                    touched.push_back({.key = ka + kb[j], .slot = ia + at});
                }
                impl::slot_add_mul_factors<C>(row[at], ca, fbp[j]);
            }
        }

        // the box fits every exponent and degree, so the raw sums of keys above need no overflow check
        if (touched.size() <= 24)
        {
            // a short list sorts fastest by insertion
            for (isize i = 1; i < touched.size(); ++i)
            {
                auto const t = touched[i];
                auto j = i;
                for (; j > 0 && Vars::greater(t.key, touched[j - 1].key); --j)
                    touched[j] = touched[j - 1];
                touched[j] = t;
            }
        }
        else
            cc::sort(touched, [](touched_slot const& x, touched_slot const& y) { return Vars::greater(x.key, y.key); });
        r._keys.reserve(touched.size());
        r._coeffs.reserve(touched.size());
        for (auto const& t : touched)
        {
            seen[t.slot] = 0;
            auto& c = dense[t.slot];
            if (impl::slot_is_zero<C>(c))
                continue;
            r._push(t.key, impl::slot_take<C>(c));
        }
        return true;
    }

    /// The product by Monagan and Pearce's heap: a row per term of the smaller factor, each walking the larger,
    /// so the pairs' monomials come out greatest first and each run of equal ones is accumulated in place, sorted without a sort.
    template <class S>
    [[nodiscard]] static multi_polynomial _product_heap(multi_polynomial const& a, multi_polynomial const& b)
    {
        auto const& rows = a.term_count() <= b.term_count() ? a : b;
        auto const& cols = a.term_count() <= b.term_count() ? b : a;
        auto const width = cols.term_count();

        struct entry
        {
            key_t key;
            int i;
            int j;
        };
        // Rows enter lazily, as Monagan and Pearce's do: row i + 1's first product ranks no higher than row i's,
        // so it is inserted when row i's first is taken, and the heap holds only the rows begun.
        auto const height = rows.term_count();
        cc::vector<entry> heap;
        heap.reserve(height);
        heap.push_back({.key = Vars::multiply(rows._keys[0], cols._keys[0]), .i = 0, .j = 0});

        // both sifts carry the moving entry in a hole, one store a level rather than a swap, through raw pointers
        auto const sift_up = [&](isize c)
        {
            auto* const h = heap.data();
            auto const x = h[c];
            while (c > 0)
            {
                auto const p = (c - 1) / 2;
                if (!Vars::greater(x.key, h[p].key))
                    break;
                h[c] = h[p];
                c = p;
            }
            h[c] = x;
        };
        auto const sift_down = [&]
        {
            auto* const h = heap.data();
            auto const n = heap.size();
            auto const x = h[0];
            isize p = 0;
            while (true)
            {
                auto c = 2 * p + 1;
                if (c >= n)
                    break;
                if (c + 1 < n && Vars::greater(h[c + 1].key, h[c].key))
                    ++c;
                if (!Vars::greater(h[c].key, x.key))
                    break;
                h[p] = h[c];
                p = c;
            }
            h[p] = x;
        };

        auto const fr = impl::slot_factors<C, S>(rows._coeffs);
        auto const fc = impl::slot_factors<C, S>(cols._coeffs);
        multi_polynomial r;
        while (!heap.empty())
        {
            auto const k = heap[0].key;
            auto acc = impl::zero_slot<C, S>();
            do
            {
                auto& top = heap[0];
                auto const i = top.i;
                auto const j = top.j;
                impl::slot_add_mul_factors<C>(acc, fr[i], fc[j]);
                if (j + 1 < width)
                {
                    top.j = j + 1;
                    top.key = Vars::multiply(rows._keys[i], cols._keys[j + 1]);
                }
                else
                {
                    top = heap.back();
                    heap.remove_back();
                }
                if (!heap.empty())
                    sift_down();
                if (j == 0 && i + 1 < height)
                {
                    heap.push_back({.key = Vars::multiply(rows._keys[i + 1], cols._keys[0]), .i = i + 1, .j = 0});
                    sift_up(heap.size() - 1);
                }
            } while (!heap.empty() && heap[0].key == k);
            if (!impl::slot_is_zero<C>(acc))
                r._push(k, impl::slot_take<C>(acc));
        }
        return r;
    }

    /// Sorts the terms by the order, their monomials distinct already.
    /// The keys sort inline beside their term's index, so a compare reads two keys and no indirection.
    void _sort_terms()
    {
        struct keyed
        {
            key_t key;
            int t;
        };
        cc::vector<keyed> order;
        order.reserve(term_count());
        for (auto t = 0; t < term_count(); ++t)
            order.push_back({.key = _keys[t], .t = t});
        cc::sort(order, [](keyed const& x, keyed const& y) { return Vars::greater(x.key, y.key); });
        cc::vector<C> coeffs;
        coeffs.reserve(term_count());
        for (auto i = 0; i < term_count(); ++i)
        {
            _keys[i] = order[i].key;
            coeffs.push_back(cc::move(_coeffs[order[i].t]));
        }
        _coeffs = cc::move(coeffs);
    }

    /// Terms in any order, possibly repeated, made sorted and combined.
    [[nodiscard]] static multi_polynomial _from_unsorted(cc::vector<key_t> keys, cc::vector<C> coeffs)
    {
        cc::vector<int> order;
        for (auto t = 0; t < int(keys.size()); ++t)
            order.push_back(t);
        cc::sort(order, [&](int x, int y) { return Vars::greater(keys[x], keys[y]); });
        multi_polynomial r;
        for (isize s = 0; s < order.size();)
        {
            auto acc = cc::move(coeffs[order[s]]);
            auto e = s + 1;
            for (; e < order.size() && keys[order[e]] == keys[order[s]]; ++e)
                acc += coeffs[order[e]];
            if (!impl::coefficient_is_zero(acc))
                r._push(keys[order[s]], cc::move(acc));
            s = e;
        }
        return r;
    }

    cc::vector<key_t> _keys;
    cc::vector<C> _coeffs;
};

template <class C, class Vars>
cc::string tg::multi_polynomial<C, Vars>::to_string(cc::span<cc::string_view const> names) const
{
    if (is_zero())
        return "0";
    cc::string s;
    for (auto t = 0; t < term_count(); ++t)
    {
        auto monomial = cc::string();
        for (auto v = 0; v < variable_count; ++v)
        {
            auto const e = Vars::exponent(_keys[t], v);
            if (e == 0)
                continue;
            if (!monomial.empty())
                monomial += "*";
            if (v < int(names.size()))
                monomial += names[v];
            else
                monomial += cc::format("x{}", v);
            if (e > 1)
                monomial += cc::format("^{}", e);
        }
        auto cs = impl::coefficient_string(_coeffs[t]);
        auto const body = cc::string_view(cs).subview(cs.starts_with("-") ? 1 : 0);
        if (!monomial.empty() && (body.contains(" + ") || body.contains(" - ")))
            cs = "(" + cs + ")";
        auto const negative = cs.starts_with("-");
        auto term = cc::string(cc::string_view(cs).subview(negative ? 1 : 0));
        if (!monomial.empty())
            term = term == "1" ? monomial : term + "*" + monomial;
        if (s.empty())
            s = negative ? "-" + term : term;
        else
            s += (negative ? " - " : " + ") + term;
    }
    return s;
}

/// A multi_polynomial is a scalar to tg as a polynomial is: exact when its coefficients are, never a field.
template <class C, class Vars>
struct tg::scalar_traits<tg::multi_polynomial<C, Vars>>
{
    using T = tg::multi_polynomial<C, Vars>;

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
    [[nodiscard]] static bool is_one(T const& x) { return x.is_constant() && !x.is_zero() && x.coefficient(0) == C(1); }
};
