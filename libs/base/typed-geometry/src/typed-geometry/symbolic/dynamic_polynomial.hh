#pragma once

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <typed-geometry/scalar/bigint/bigint.hh>
#include <typed-geometry/symbolic/multi_polynomial.hh>
#include <typed-geometry/symbolic/symbol.hh>

#include <initializer_list>
#include <type_traits>

// tg::multi_polynomial<C, tg::dynamic_vars<Order>>: a sparse polynomial over named variables, for computer algebra.
// libs/base/typed-geometry/docs/modules/symbolic.md has the design, and libs/base/typed-geometry/cheat-sheet.md the surface.

/// Variables known only at run time, as tg::symbol, ordered by Order as a vars<> is; each polynomial carries its own.
template <class Order>
struct tg::dynamic_vars
{
    using order = Order;
};

namespace tg::impl
{
/// A vars<> packing with N known only at run time: 16-bit exponent fields, a guard bit each, most significant first.
template <class Order>
struct dynamic_layout
{
    static constexpr int bits = 16;
    static constexpr int fields_per_word = 4;
    static constexpr i64 max_exponent = (i64(1) << (bits - 1)) - 1;

    int n = 0;
    int words = 1;
    cc::vector<u64> guard;
    cc::vector<u64> compare_mask;

    explicit dynamic_layout(int variables) : n(variables)
    {
        auto const fields = n + (Order::graded ? 1 : 0);
        words = fields == 0 ? 1 : (fields + fields_per_word - 1) / fields_per_word;
        guard.resize_to_filled(words, u64(0));
        compare_mask.resize_to_filled(words, u64(0));
        for (auto f = 0; f < fields; ++f)
        {
            guard[f / fields_per_word] |= (u64(1) << (bits - 1)) << shift_of(f);
            if (Order::compare_reversed && f >= (Order::graded ? 1 : 0))
                compare_mask[f / fields_per_word] |= ((u64(1) << bits) - 1) << shift_of(f);
        }
    }

    [[nodiscard]] int field_of(int v) const
    {
        return (Order::graded ? 1 : 0) + (Order::variables_reversed ? n - 1 - v : v);
    }
    [[nodiscard]] static int shift_of(int field) { return 64 - bits * (field % fields_per_word + 1); }

    /// Packs n exponents into out[0, words).
    void pack(int const* e, u64* out) const
    {
        for (auto w = 0; w < words; ++w)
            out[w] = 0;
        i64 degree = 0;
        for (auto v = 0; v < n; ++v)
        {
            CC_ASSERT_ALWAYS(0 <= e[v] && e[v] <= max_exponent, "tg::dynamic_vars: an exponent past the 16-bit field");
            auto const f = field_of(v);
            out[f / fields_per_word] |= u64(e[v]) << shift_of(f);
            degree += e[v];
        }
        if constexpr (Order::graded)
        {
            CC_ASSERT_ALWAYS(degree <= max_exponent, "tg::dynamic_vars: a total degree past the 16-bit field");
            out[0] |= u64(degree) << shift_of(0);
        }
    }

    [[nodiscard]] int exponent(u64 const* k, int v) const
    {
        auto const f = field_of(v);
        return int((k[f / fields_per_word] >> shift_of(f)) & ((u64(1) << bits) - 1));
    }

    void multiply(u64 const* a, u64 const* b, u64* out) const
    {
        u64 overflow = 0;
        for (auto w = 0; w < words; ++w)
        {
            out[w] = a[w] + b[w];
            overflow |= out[w] & guard[w];
        }
        CC_ASSERT_ALWAYS(overflow == 0, "tg::dynamic_vars: a product's exponent past the 16-bit field");
    }

    [[nodiscard]] bool greater(u64 const* a, u64 const* b) const
    {
        for (auto w = 0; w < words; ++w)
        {
            auto const x = a[w] ^ compare_mask[w];
            auto const y = b[w] ^ compare_mask[w];
            if (x != y)
                return x > y;
        }
        return false;
    }

    [[nodiscard]] bool equal(u64 const* a, u64 const* b) const
    {
        for (auto w = 0; w < words; ++w)
            if (a[w] != b[w])
                return false;
        return true;
    }
};

/// An integer literal of the parser as a C: through bigint where C is built from one, i64 otherwise.
template <class C>
[[nodiscard]] C coefficient_from_bigint(bigint const& v)
{
    if constexpr (std::is_constructible_v<C, bigint const&>)
        return C(v);
    else
        return C(v.to_i64());
}
} // namespace tg::impl

/// A sum of terms c * x^e * y^f * ... over named variables, for computer algebra: tg::multi_polynomial's interface, by symbol.
/// Each polynomial carries the sorted list of variables its terms are packed over; operands over different lists are repacked
/// onto their union first, which keeps every term's order, a variable with exponent zero everywhere ranking nothing.
/// Exponents are 16-bit fields, checked in every build; products go through the heap, there being no static box to fill.
template <class C, class Order>
struct tg::multi_polynomial<C, tg::dynamic_vars<Order>>
{
    using coefficient_t = C;
    using vars_t = dynamic_vars<Order>;

    multi_polynomial() = default;

    template <class S>
        requires impl::multi_polynomial_scalar_of<S, C>
    multi_polynomial(S const& c)
    {
        C v(c);
        if (!impl::coefficient_is_zero(v))
        {
            _keys.push_back(0);
            _coeffs.push_back(cc::move(v));
        }
    }

    /// The variable x.
    [[nodiscard]] static multi_polynomial variable(symbol x)
    {
        multi_polynomial p;
        p._vars.push_back(x);
        auto const layout = p._layout();
        int const e = 1;
        p._keys.resize_to_filled(layout.words, u64(0));
        layout.pack(&e, p._keys.data());
        p._coeffs.push_back(C(1));
        return p;
    }

    /// The polynomial written in text: integers, names, + - * ^ with a non-negative integer power, and parentheses.
    /// Empty when the text is malformed.
    [[nodiscard]] static cc::optional<multi_polynomial> parse(cc::string_view text);

    // observers

    [[nodiscard]] bool is_zero() const { return _coeffs.empty(); }
    [[nodiscard]] bool is_constant() const
    {
        if (_coeffs.empty())
            return true;
        if (_coeffs.size() > 1)
            return false;
        for (auto w = 0; w < _words(); ++w)
            if (_keys[w] != 0)
                return false;
        return true;
    }
    [[nodiscard]] int term_count() const { return int(_coeffs.size()); }
    /// The variables the terms are packed over, ascending by name; a variable may have dropped out of every term.
    [[nodiscard]] cc::span<symbol const> variables() const { return _vars; }

    [[nodiscard]] C const& coefficient(int t) const { return _coeffs[t]; }
    /// Term t's exponent of x, 0 for a variable it is not over.
    [[nodiscard]] int exponent(int t, symbol x) const
    {
        auto const v = _index_of(x);
        return v < 0 ? 0 : _layout().exponent(_key(t), v);
    }
    [[nodiscard]] C const& leading_coefficient() const
    {
        CC_ASSERT(!is_zero(), "tg::multi_polynomial: the zero polynomial has no leading term");
        return _coeffs[0];
    }

    /// The highest exponent of x, -1 for the zero polynomial.
    [[nodiscard]] int degree(symbol x) const
    {
        if (is_zero())
            return -1;
        auto const v = _index_of(x);
        if (v < 0)
            return 0;
        auto const layout = _layout();
        auto d = 0;
        for (auto t = 0; t < term_count(); ++t)
            d = cc::max(d, layout.exponent(_key(t), v));
        return d;
    }
    [[nodiscard]] int total_degree() const
    {
        if (is_zero())
            return -1;
        auto const layout = _layout();
        auto d = 0;
        for (auto t = 0; t < term_count(); ++t)
        {
            auto s = 0;
            for (auto v = 0; v < layout.n; ++v)
                s += layout.exponent(_key(t), v);
            d = cc::max(d, s);
        }
        return d;
    }

    /// The sign for infinitesimal variables: the leading coefficient's.
    [[nodiscard]] int sign() const
        requires std::is_same_v<Order, infinitesimal_order>
    {
        return is_zero() ? 0 : impl::coefficient_sign(_coeffs[0]);
    }

    /// The value with every variable assigned, in X where X is built from C, in C otherwise.
    template <class X>
    [[nodiscard]] auto evaluate(cc::span<assignment<X> const> values) const
    {
        using R = std::conditional_t<std::is_constructible_v<X, C const&>, X, C>;
        auto const layout = _layout();
        cc::vector<R> at;
        for (auto const& x : _vars)
        {
            auto found = false;
            for (auto const& a : values)
                if (a.variable == x)
                {
                    at.push_back(R(a.value));
                    found = true;
                    break;
                }
            CC_ASSERT_ALWAYS(found, "tg::multi_polynomial::evaluate: a variable without a value");
        }
        auto r = R(0);
        for (auto t = 0; t < term_count(); ++t)
        {
            auto term = R(_coeffs[t]);
            for (auto v = 0; v < layout.n; ++v)
                for (auto e = layout.exponent(_key(t), v); e > 0; --e)
                    term = term * at[v];
            r += term;
        }
        return r;
    }
    [[nodiscard]] C operator()(std::initializer_list<assignment<C>> values) const
    {
        return evaluate(cc::span<assignment<C> const>(values.begin(), isize(values.size())));
    }

    /// The terms in order, as "3*x^2*y - y + 1".
    [[nodiscard]] cc::string to_string() const;

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
        return _sum(a, b, false);
    }
    [[nodiscard]] friend multi_polynomial operator-(multi_polynomial const& a, multi_polynomial const& b)
    {
        return _sum(a, b, true);
    }
    [[nodiscard]] friend multi_polynomial operator*(multi_polynomial const& a, multi_polynomial const& b)
    {
        return _product(a, b);
    }
    multi_polynomial& operator+=(multi_polynomial const& b) { return *this = _sum(*this, b, false); }
    multi_polynomial& operator-=(multi_polynomial const& b) { return *this = _sum(*this, b, true); }
    multi_polynomial& operator*=(multi_polynomial const& b) { return *this = _product(*this, b); }

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

    /// Equal as polynomials, whatever variables each was packed over.
    [[nodiscard]] friend bool operator==(multi_polynomial const& a, multi_polynomial const& b)
    {
        if (a.term_count() != b.term_count())
            return false;
        if (!_same(a._vars, b._vars))
        {
            auto const vars = _union(a._vars, b._vars);
            return _over(a, vars) == _over(b, vars);
        }
        for (isize i = 0; i < a._keys.size(); ++i)
            if (a._keys[i] != b._keys[i])
                return false;
        for (auto t = 0; t < a.term_count(); ++t)
            if (!(a._coeffs[t] == b._coeffs[t]))
                return false;
        return true;
    }

    /// d/dx; a term's order survives division by x, so no sort.
    [[nodiscard]] friend multi_polynomial derivative(multi_polynomial const& p, symbol x)
    {
        auto const v = p._index_of(x);
        multi_polynomial r;
        r._vars = p._vars;
        if (v < 0)
            return r;
        auto const layout = p._layout();
        cc::vector<int> e;
        e.resize_to_filled(layout.n, 0);
        cc::vector<u64> key;
        key.resize_to_filled(layout.words, u64(0));
        for (auto t = 0; t < p.term_count(); ++t)
        {
            p._unpack(t, e.data());
            if (e[v] == 0)
                continue;
            auto c = p._coeffs[t] * C(e[v]);
            if (impl::coefficient_is_zero(c))
                continue;
            --e[v];
            layout.pack(e.data(), key.data());
            r._push(key.data(), layout.words, cc::move(c));
        }
        return r;
    }

    /// p with x replaced by q, itself a polynomial or a constant.
    [[nodiscard]] friend multi_polynomial substitute(multi_polynomial const& p, symbol x, multi_polynomial const& q)
    {
        auto const v = p._index_of(x);
        if (v < 0)
            return p;
        auto const layout = p._layout();
        cc::vector<multi_polynomial> powers;
        powers.push_back(multi_polynomial(C(1)));
        multi_polynomial r;
        cc::vector<int> e;
        e.resize_to_filled(layout.n, 0);
        for (auto t = 0; t < p.term_count(); ++t)
        {
            p._unpack(t, e.data());
            auto const k = e[v];
            while (int(powers.size()) <= k)
                powers.push_back(powers.back() * q);
            e[v] = 0;
            multi_polynomial rest;
            rest._vars = p._vars;
            rest._keys.resize_to_filled(layout.words, u64(0));
            layout.pack(e.data(), rest._keys.data());
            rest._coeffs.push_back(p._coeffs[t]);
            r += rest * powers[k];
        }
        return r;
    }

private:
    [[nodiscard]] impl::dynamic_layout<Order> _layout() const { return impl::dynamic_layout<Order>(int(_vars.size())); }
    [[nodiscard]] int _words() const { return _layout().words; }
    [[nodiscard]] u64 const* _key(int t) const { return _keys.data() + isize(t) * _words(); }

    [[nodiscard]] int _index_of(symbol x) const
    {
        for (isize v = 0; v < _vars.size(); ++v)
            if (_vars[v] == x)
                return int(v);
        return -1;
    }

    void _unpack(int t, int* e) const
    {
        auto const layout = _layout();
        for (auto v = 0; v < layout.n; ++v)
            e[v] = layout.exponent(_key(t), v);
    }

    void _push(u64 const* key, int words, C c)
    {
        for (auto w = 0; w < words; ++w)
            _keys.push_back(key[w]);
        _coeffs.push_back(cc::move(c));
    }

    void _scale(C const& s)
    {
        auto const words = _words();
        multi_polynomial r;
        r._vars = _vars;
        for (auto t = 0; t < term_count(); ++t)
        {
            auto c = _coeffs[t] * s;
            if (!impl::coefficient_is_zero(c))
                r._push(_key(t), words, cc::move(c));
        }
        *this = cc::move(r);
    }

    [[nodiscard]] static bool _same(cc::span<symbol const> a, cc::span<symbol const> b)
    {
        if (a.size() != b.size())
            return false;
        for (isize i = 0; i < a.size(); ++i)
            if (!(a[i] == b[i]))
                return false;
        return true;
    }

    /// The sorted union of two sorted variable lists.
    [[nodiscard]] static cc::vector<symbol> _union(cc::span<symbol const> a, cc::span<symbol const> b)
    {
        cc::vector<symbol> u;
        isize i = 0;
        isize j = 0;
        while (i < a.size() || j < b.size())
        {
            if (j == b.size() || (i < a.size() && a[i] < b[j]))
                u.push_back(a[i++]);
            else if (i == a.size() || b[j] < a[i])
                u.push_back(b[j++]);
            else
            {
                u.push_back(a[i++]);
                ++j;
            }
        }
        return u;
    }

    /// p repacked over vars, a superset of its own; the terms keep their order, an added variable's zero ranking nothing.
    [[nodiscard]] static multi_polynomial _over(multi_polynomial const& p, cc::vector<symbol> const& vars)
    {
        if (_same(p._vars, vars))
            return p;
        multi_polynomial r;
        r._vars = vars;
        auto const from = p._layout();
        auto const to = r._layout();
        cc::vector<int> map;
        for (auto const& x : p._vars)
        {
            isize k = 0;
            while (!(vars[k] == x))
                ++k;
            map.push_back(int(k));
        }
        cc::vector<int> e;
        e.resize_to_filled(to.n, 0);
        r._keys.resize_to_filled(isize(p.term_count()) * to.words, u64(0));
        for (auto t = 0; t < p.term_count(); ++t)
        {
            for (auto& x : e)
                x = 0;
            for (auto v = 0; v < from.n; ++v)
                e[map[v]] = from.exponent(p._key(t), v);
            to.pack(e.data(), r._keys.data() + isize(t) * to.words);
        }
        r._coeffs = p._coeffs;
        return r;
    }

    [[nodiscard]] static multi_polynomial _sum(multi_polynomial const& a0, multi_polynomial const& b0, bool subtract)
    {
        auto const vars = _union(a0._vars, b0._vars);
        auto const a = _over(a0, vars);
        auto const b = _over(b0, vars);
        auto const layout = a._layout();
        auto const words = layout.words;
        multi_polynomial r;
        r._vars = vars;
        auto i = 0;
        auto j = 0;
        while (i < a.term_count() && j < b.term_count())
        {
            if (layout.greater(a._key(i), b._key(j)))
            {
                r._push(a._key(i), words, a._coeffs[i]);
                ++i;
            }
            else if (layout.greater(b._key(j), a._key(i)))
            {
                r._push(b._key(j), words, subtract ? -b._coeffs[j] : b._coeffs[j]);
                ++j;
            }
            else
            {
                auto c = subtract ? a._coeffs[i] - b._coeffs[j] : a._coeffs[i] + b._coeffs[j];
                if (!impl::coefficient_is_zero(c))
                    r._push(a._key(i), words, cc::move(c));
                ++i;
                ++j;
            }
        }
        for (; i < a.term_count(); ++i)
            r._push(a._key(i), words, a._coeffs[i]);
        for (; j < b.term_count(); ++j)
            r._push(b._key(j), words, subtract ? -b._coeffs[j] : b._coeffs[j]);
        return r;
    }

    /// The product by Monagan and Pearce's heap over the union of the variables, each coefficient summed in one slot.
    [[nodiscard]] static multi_polynomial _product(multi_polynomial const& a0, multi_polynomial const& b0)
    {
        if (a0.is_zero() || b0.is_zero())
            return {};
        auto const vars = _union(a0._vars, b0._vars);
        auto const a = _over(a0, vars);
        auto const b = _over(b0, vars);
        auto const& rows = a.term_count() <= b.term_count() ? a : b;
        auto const& cols = a.term_count() <= b.term_count() ? b : a;
        return impl::with_slot<C>(impl::widest_limbs<C>(a._coeffs, b._coeffs),
                                  [&]<class S>() -> multi_polynomial { return _product_heap<S>(rows, cols); });
    }

    template <class S>
    [[nodiscard]] static multi_polynomial _product_heap(multi_polynomial const& rows, multi_polynomial const& cols)
    {
        auto const layout = rows._layout();
        auto const words = layout.words;
        auto const width = cols.term_count();
        auto const fr = impl::slot_factors<C, S>(rows._coeffs);
        auto const fc = impl::slot_factors<C, S>(cols._coeffs);

        // entry e's key at keys[e * words]; the rows' first products descend as the rows do, a max-heap in row order
        struct entry
        {
            int i;
            int j;
        };
        cc::vector<entry> heap;
        cc::vector<u64> keys;
        keys.resize_to_filled(isize(rows.term_count()) * words, u64(0));
        for (auto i = 0; i < rows.term_count(); ++i)
        {
            heap.push_back({.i = i, .j = 0});
            layout.multiply(rows._key(i), cols._key(0), keys.data() + isize(i) * words);
        }
        // a heap slot's key lives beside it, so swapping entries swaps their keys too
        auto const key_of = [&](isize h) { return keys.data() + h * words; };
        auto const swap_slots = [&](isize x, isize y)
        {
            cc::swap(heap[x], heap[y]);
            for (auto w = 0; w < words; ++w)
                cc::swap(key_of(x)[w], key_of(y)[w]);
        };
        auto const sift_down = [&]
        {
            auto const n = heap.size();
            isize p = 0;
            while (true)
            {
                auto c = 2 * p + 1;
                if (c >= n)
                    break;
                if (c + 1 < n && layout.greater(key_of(c + 1), key_of(c)))
                    ++c;
                if (!layout.greater(key_of(c), key_of(p)))
                    break;
                swap_slots(c, p);
                p = c;
            }
        };

        multi_polynomial r;
        r._vars = rows._vars;
        cc::vector<u64> k;
        k.resize_to_filled(words, u64(0));
        while (!heap.empty())
        {
            for (auto w = 0; w < words; ++w)
                k[w] = key_of(0)[w];
            auto acc = impl::zero_slot<C, S>();
            do
            {
                auto& top = heap[0];
                impl::slot_add_mul_factors<C>(acc, fr[top.i], fc[top.j]);
                if (++top.j < width)
                {
                    layout.multiply(rows._key(top.i), cols._key(top.j), key_of(0));
                }
                else
                {
                    auto const last = heap.size() - 1;
                    if (last > 0)
                        swap_slots(0, last);
                    heap.remove_back();
                    keys.resize_down_to(heap.size() * words);
                }
                sift_down();
            } while (!heap.empty() && layout.equal(key_of(0), k.data()));
            if (!impl::slot_is_zero<C>(acc))
                r._push(k.data(), words, impl::slot_take<C>(acc));
        }
        return r;
    }

    cc::vector<symbol> _vars;
    cc::vector<u64> _keys;
    cc::vector<C> _coeffs;
};

template <class C, class Order>
cc::string tg::multi_polynomial<C, tg::dynamic_vars<Order>>::to_string() const
{
    if (is_zero())
        return "0";
    auto const layout = _layout();
    cc::string s;
    for (auto t = 0; t < term_count(); ++t)
    {
        auto monomial = cc::string();
        for (auto v = 0; v < layout.n; ++v)
        {
            auto const e = layout.exponent(_key(t), v);
            if (e == 0)
                continue;
            if (!monomial.empty())
                monomial += "*";
            monomial += _vars[v].name();
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

namespace tg::impl
{
/// Recursive descent over a polynomial's text: expr = term (('+' | '-') term)*, term = power ('*' power)*,
/// power = atom ('^' integer)?, atom = integer | name | '(' expr ')' | '-' atom.
template <class P>
struct polynomial_parser
{
    cc::string_view text;
    isize at = 0;
    bool failed = false;

    void skip()
    {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n'))
            ++at;
    }
    [[nodiscard]] bool eat(char c)
    {
        skip();
        if (at < text.size() && text[at] == c)
        {
            ++at;
            return true;
        }
        return false;
    }
    [[nodiscard]] static bool is_name_start(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    [[nodiscard]] static bool is_digit(char c) { return c >= '0' && c <= '9'; }

    P expr()
    {
        auto r = term();
        while (!failed)
        {
            if (eat('+'))
                r += term();
            else if (eat('-'))
                r -= term();
            else
                break;
        }
        return r;
    }
    P term()
    {
        auto r = power();
        while (!failed && eat('*'))
            r *= power();
        return r;
    }
    P power()
    {
        auto base = atom();
        if (failed || !eat('^'))
            return base;
        skip();
        auto const start = at;
        while (at < text.size() && is_digit(text[at]))
            ++at;
        if (at == start || at - start > 5)
        {
            failed = true;
            return {};
        }
        auto e = 0;
        for (auto i = start; i < at; ++i)
            e = e * 10 + (text[i] - '0');
        auto r = P(1);
        for (auto i = 0; i < e; ++i)
            r *= base;
        return r;
    }
    P atom()
    {
        skip();
        if (at >= text.size())
        {
            failed = true;
            return {};
        }
        if (eat('('))
        {
            auto r = expr();
            if (!eat(')'))
                failed = true;
            return r;
        }
        if (eat('-'))
            return -atom();
        auto const start = at;
        if (is_digit(text[at]))
        {
            while (at < text.size() && is_digit(text[at]))
                ++at;
            auto const v = bigint::parse(text.subview(cc::offset_size{.offset = start, .size = at - start}));
            if (!v.has_value())
            {
                failed = true;
                return {};
            }
            return P(coefficient_from_bigint<typename P::coefficient_t>(v.value()));
        }
        if (is_name_start(text[at]))
        {
            while (at < text.size() && (is_name_start(text[at]) || is_digit(text[at])))
                ++at;
            return P::variable(symbol(text.subview(cc::offset_size{.offset = start, .size = at - start})));
        }
        failed = true;
        return {};
    }
};
} // namespace tg::impl

template <class C, class Order>
cc::optional<tg::multi_polynomial<C, tg::dynamic_vars<Order>>> tg::multi_polynomial<C, tg::dynamic_vars<Order>>::parse(
    cc::string_view text)
{
    auto parser = impl::polynomial_parser<multi_polynomial>{.text = text};
    auto p = parser.expr();
    parser.skip();
    if (parser.failed || parser.at != text.size())
        return {};
    return p;
}

/// A dynamic polynomial is a scalar to tg as a static one is.
template <class C, class Order>
struct tg::scalar_traits<tg::multi_polynomial<C, tg::dynamic_vars<Order>>>
{
    using T = tg::multi_polynomial<C, tg::dynamic_vars<Order>>;

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
