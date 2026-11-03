#pragma once

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
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
    multi_polynomial& operator+=(multi_polynomial const& b) { return *this = _merge(*this, b, false); }
    multi_polynomial& operator-=(multi_polynomial const& b) { return *this = _merge(*this, b, true); }
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

    /// The product: every pair's monomial, sorted, and each run of equal ones accumulated in place.
    /// A single-term factor keeps the other's order, the order being compatible with multiplication, so it skips the sort.
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

        struct pair_product
        {
            key_t key;
            int i;
            int j;
        };
        cc::vector<pair_product> pairs;
        pairs.reserve(isize(a.term_count()) * b.term_count());
        for (auto i = 0; i < a.term_count(); ++i)
            for (auto j = 0; j < b.term_count(); ++j)
                pairs.push_back({.key = Vars::multiply(a._keys[i], b._keys[j]), .i = i, .j = j});
        cc::sort(pairs, [](pair_product const& x, pair_product const& y) { return Vars::greater(x.key, y.key); });

        for (isize s = 0; s < pairs.size();)
        {
            auto acc = C(0);
            auto e = s;
            for (; e < pairs.size() && pairs[e].key == pairs[s].key; ++e)
                impl::add_mul(acc, a._coeffs[pairs[e].i], b._coeffs[pairs[e].j]);
            if (!impl::coefficient_is_zero(acc))
                r._push(pairs[s].key, cc::move(acc));
            s = e;
        }
        return r;
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
