#pragma once

#include <clean-core/string/string.hh>
#include <typed-geometry/scalar/bigint/bigint.hh>

#include <compare>
#include <type_traits>

// tg::bigrat: an exact rational over two bigints, always reduced; tg::bigrat_unreduced defers the reduction.
// libs/base/typed-geometry/docs/modules/scalar.md has the design, and libs/base/typed-geometry/cheat-sheet.md the surface.

/// Exact + - * / and comparison: num / den with den positive and coprime to num, after every operation.
/// The denominator is held as a bigint whose zero stands for 1, so an integer-valued rational holds no second node
/// and adds, subtracts and multiplies at bigint's cost.
/// Each operation reduces by the smallest gcds it can: a sum by the denominators' gcd, a product by the two cross gcds.
struct tg::bigrat
{
    bigrat() = default;

    template <class I>
        requires(std::is_integral_v<I> && !std::is_same_v<I, bool>)
    bigrat(I v) : _num(v)
    {
    }

    explicit bigrat(bigint n) : _num(cc::move(n)) {}

    /// num / den reduced; den != 0, of either sign.
    bigrat(bigint num, bigint const& den);

    /// x exactly, a finite double being a dyadic rational; x must be finite.
    [[nodiscard]] static bigrat from_f64(f64 x);

    // observers

    [[nodiscard]] bool is_zero() const { return _num.is_zero(); }
    [[nodiscard]] bool is_negative() const { return _num.is_negative(); }
    [[nodiscard]] int sign() const { return _num.sign(); }
    [[nodiscard]] bool is_integer() const { return _den.is_zero(); }
    [[nodiscard]] bigint const& numerator() const { return _num; }
    /// The denominator, positive; a copy, since an integer holds none.
    [[nodiscard]] bigint denominator() const { return _den.is_zero() ? bigint(1) : _den; }
    /// Rounded to nearest, ties to even, and infinite past the double range.
    [[nodiscard]] f64 to_f64() const;
    /// "num/den" in decimal, or "num" for an integer.
    [[nodiscard]] cc::string to_string() const;

    // arithmetic
    // Every assign_* reuses *this's nodes, and an operand may be *this.

    void negate() { _num.negate(); }
    void assign_add(bigrat const& a, bigrat const& b) { _assign_sum(a, b, false); }
    void assign_sub(bigrat const& a, bigrat const& b) { _assign_sum(a, b, true); }
    void assign_mul(bigrat const& a, bigrat const& b);
    /// a / b, b != 0.
    void assign_div(bigrat const& a, bigrat const& b);

    [[nodiscard]] friend bigrat operator-(bigrat a)
    {
        a.negate();
        return a;
    }
    [[nodiscard]] friend bigrat abs(bigrat a)
    {
        if (a.is_negative())
            a.negate();
        return a;
    }
    [[nodiscard]] friend bigrat operator+(bigrat const& a, bigrat const& b)
    {
        bigrat r;
        r.assign_add(a, b);
        return r;
    }
    [[nodiscard]] friend bigrat operator-(bigrat const& a, bigrat const& b)
    {
        bigrat r;
        r.assign_sub(a, b);
        return r;
    }
    [[nodiscard]] friend bigrat operator*(bigrat const& a, bigrat const& b)
    {
        bigrat r;
        r.assign_mul(a, b);
        return r;
    }
    [[nodiscard]] friend bigrat operator/(bigrat const& a, bigrat const& b)
    {
        bigrat r;
        r.assign_div(a, b);
        return r;
    }
    // an rvalue operand's nodes become the result's
    [[nodiscard]] friend bigrat operator+(bigrat&& a, bigrat const& b)
    {
        a.assign_add(a, b);
        return cc::move(a);
    }
    [[nodiscard]] friend bigrat operator-(bigrat&& a, bigrat const& b)
    {
        a.assign_sub(a, b);
        return cc::move(a);
    }
    [[nodiscard]] friend bigrat operator*(bigrat&& a, bigrat const& b)
    {
        a.assign_mul(a, b);
        return cc::move(a);
    }
    [[nodiscard]] friend bigrat operator/(bigrat&& a, bigrat const& b)
    {
        a.assign_div(a, b);
        return cc::move(a);
    }
    bigrat& operator+=(bigrat const& b)
    {
        assign_add(*this, b);
        return *this;
    }
    bigrat& operator-=(bigrat const& b)
    {
        assign_sub(*this, b);
        return *this;
    }
    bigrat& operator*=(bigrat const& b)
    {
        assign_mul(*this, b);
        return *this;
    }
    bigrat& operator/=(bigrat const& b)
    {
        assign_div(*this, b);
        return *this;
    }

    // comparison
    // Reduced, so equality is the parts' equality; order is by signs, then bit widths, then the cross products.

    friend int compare(bigrat const& a, bigrat const& b);
    [[nodiscard]] friend bool operator==(bigrat const& a, bigrat const& b)
    {
        return a._num == b._num && a._den == b._den;
    }
    [[nodiscard]] friend std::strong_ordering operator<=>(bigrat const& a, bigrat const& b)
    {
        auto const c = compare(a, b);
        return c < 0 ? std::strong_ordering::less : c > 0 ? std::strong_ordering::greater : std::strong_ordering::equal;
    }

private:
    friend struct bigrat_unreduced;

    bigint _num;
    /// Zero stands for 1; otherwise greater than 1 and coprime to _num.
    bigint _den;

    void _assign_sum(bigrat const& a, bigrat const& b, bool subtract);
    /// *this = (p/q) (r/s) by the cross gcds: p nonzero, q positive, zero standing for 1 in q, r and s, and s of either sign.
    void _assign_cross(bigint const& p, bigint const& q, bigint const& r, bigint const& s);
    /// Divides _num and _den by their gcd and makes _den positive, a denominator of 1 becoming zero; _den != 0.
    void _reduce();
};

/// num / den without the gcd per operation, for a hot loop that reduces once at its end.
/// Sums and products are bigint products alone, but the parts grow with every operation the reduction defers.
/// The denominator's zero stands for 1, as in bigrat, and is otherwise positive.
struct tg::bigrat_unreduced
{
    bigrat_unreduced() = default;

    template <class I>
        requires(std::is_integral_v<I> && !std::is_same_v<I, bool>)
    bigrat_unreduced(I v) : num(v)
    {
    }

    bigrat_unreduced(bigrat const& r) : num(r._num), den(r._den) {}
    bigrat_unreduced(bigrat&& r) : num(cc::move(r._num)), den(cc::move(r._den)) {}

    bigint num;
    /// Zero stands for 1; otherwise positive.
    bigint den;

    [[nodiscard]] int sign() const { return num.sign(); }
    /// The value, reduced.
    [[nodiscard]] bigrat reduced() const&;
    [[nodiscard]] bigrat reduced() &&;

    void negate() { num.negate(); }
    void assign_add(bigrat_unreduced const& a, bigrat_unreduced const& b) { _assign_sum(a, b, false); }
    void assign_sub(bigrat_unreduced const& a, bigrat_unreduced const& b) { _assign_sum(a, b, true); }
    void assign_mul(bigrat_unreduced const& a, bigrat_unreduced const& b);

    [[nodiscard]] friend bigrat_unreduced operator+(bigrat_unreduced const& a, bigrat_unreduced const& b)
    {
        bigrat_unreduced r;
        r.assign_add(a, b);
        return r;
    }
    [[nodiscard]] friend bigrat_unreduced operator-(bigrat_unreduced const& a, bigrat_unreduced const& b)
    {
        bigrat_unreduced r;
        r.assign_sub(a, b);
        return r;
    }
    [[nodiscard]] friend bigrat_unreduced operator*(bigrat_unreduced const& a, bigrat_unreduced const& b)
    {
        bigrat_unreduced r;
        r.assign_mul(a, b);
        return r;
    }
    bigrat_unreduced& operator+=(bigrat_unreduced const& b)
    {
        assign_add(*this, b);
        return *this;
    }
    bigrat_unreduced& operator-=(bigrat_unreduced const& b)
    {
        assign_sub(*this, b);
        return *this;
    }
    bigrat_unreduced& operator*=(bigrat_unreduced const& b)
    {
        assign_mul(*this, b);
        return *this;
    }

private:
    void _assign_sum(bigrat_unreduced const& a, bigrat_unreduced const& b, bool subtract);
};

namespace tg
{
[[nodiscard]] int compare(bigrat const& a, bigrat const& b);
} // namespace tg
