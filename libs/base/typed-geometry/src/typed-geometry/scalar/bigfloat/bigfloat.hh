#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/math/wide_arith.hh>
#include <typed-geometry/scalar/bigfloat/impl/kernels.hh>

#include <compare>
#include <type_traits>

// tg::bigfloat_sbo<K>: an exact binary float, sign and magnitude times 2^exponent, for ring arithmetic that never rounds.
// libs/base/typed-geometry/docs/modules/scalar.md has the design, and libs/base/typed-geometry/cheat-sheet.md the surface.

/// One summand of bigfloat_sbo<K>::msum: a, or a * b, negated when negative.
/// It refers to its operands, which must outlive the msum it is passed to.
struct tg::bigfloat_summand
{
    impl::bigfloat_view a;
    impl::bigfloat_view b;
    bool has_b = false;
    /// The summand's own sign: the flag it was built with, times the operands' signs.
    bool negative = false;

    template <int K>
    bigfloat_summand(bigfloat_sbo<K> const& x, bool negate = false);
    template <int K>
    bigfloat_summand(bigfloat_sbo<K> const& x, bigfloat_sbo<K> const& y, bool negate = false);
};

namespace tg::impl
{
/// The limbs bigfloat_msum_kernel needs, the sign and the carries included; 0 when every summand is zero.
[[nodiscard]] i32 bigfloat_msum_limbs(cc::span<bigfloat_summand const> terms);

/// out = the sum of the terms, accumulated in one two's-complement window of n = bigfloat_msum_limbs(terms) limbs.
/// out must not overlap any operand.
[[nodiscard]] bigfloat_raw bigfloat_msum_kernel(u64* out, i32 n, cc::span<bigfloat_summand const> terms);
} // namespace tg::impl

/// Exact for + - * and comparison: no operation rounds, so the mantissa grows as the value needs.
/// K limbs live inline, and a longer mantissa spills to cc's node allocator; sizeof is 8 * K + 8.
/// The exponent is an i32 counting bits, which input doubles combined by + - * cannot exhaust.
template <int K>
struct tg::bigfloat_sbo
{
    static_assert(K >= 1, "K = 0 would cost the 16 bytes of K = 1, since the pointer needs the word anyway");

    static constexpr int inline_limbs = K;

    bigfloat_sbo() = default;

    /// Exact; v must be finite.
    bigfloat_sbo(f64 v)
    {
        auto const bits = cc::bit_cast<u64>(v);
        auto const biased = int((bits >> 52) & 0x7ff);
        CC_ASSERT(biased != 0x7ff, "a bigfloat is finite");
        auto m = bits & ((u64(1) << 52) - 1);
        auto e = -1074;
        if (biased != 0)
        {
            m |= u64(1) << 52;
            e = biased - 1075;
        }
        _set_word(m, e, (bits >> 63) != 0);
    }

    template <class I>
        requires(std::is_integral_v<I> && !std::is_same_v<I, bool>)
    bigfloat_sbo(I v)
    {
        if constexpr (std::is_signed_v<I>)
            _set_word(v < 0 ? u64(0) - u64(v) : u64(v), 0, v < 0);
        else
            _set_word(u64(v), 0, false);
    }

    /// x * 2^exponent, exactly.
    template <int Bits, bool Signed>
        requires(Bits % 64 == 0)
    [[nodiscard]] static bigfloat_sbo from_fixed_int(impl::fixed_integer<Bits, Signed> const& x, i32 exponent = 0)
    {
        constexpr int n = Bits / 64;
        auto negative = false;
        if constexpr (Signed)
            negative = x.is_negative();
        bigfloat_sbo r;
        r._reserve_discarding(n);
        auto* const d = r._data();
        // the magnitude of a negative value is ~x + 1
        auto const flip = u64(0) - u64(negative);
        auto carry = u64(negative);
        for (auto i = 0; i < n; ++i)
        {
            auto const s = cc::add_with_carry(x.limbs[i] ^ flip, 0, carry);
            d[i] = s.value;
            carry = s.carry;
        }
        r._set(impl::bigfloat_trim(d, {.size = n, .exponent = exponent, .negative = negative}));
        return r;
    }

    bigfloat_sbo(bigfloat_sbo const& o) : _exponent(o._exponent), _size(o._size), _negative(o._negative)
    {
        auto* d = _inline_limbs;
        if (o._size > K)
        {
            u32 c = 0;
            _heap_limbs = impl::bigfloat_allocate(i32(o._size), c);
            _cap_class = c;
            d = _heap_limbs;
        }
        auto const* const s = o._data();
        for (auto i = 0; i < i32(o._size); ++i)
            d[i] = s[i];
    }

    bigfloat_sbo(bigfloat_sbo&& o) noexcept { _take(o); }

    bigfloat_sbo& operator=(bigfloat_sbo const& o)
    {
        if (this != &o)
        {
            _reserve_discarding(i32(o._size));
            auto* const d = _data();
            auto const* const s = o._data();
            for (auto i = 0; i < i32(o._size); ++i)
                d[i] = s[i];
            _size = o._size;
            _exponent = o._exponent;
            _negative = o._negative;
        }
        return *this;
    }

    bigfloat_sbo& operator=(bigfloat_sbo&& o) noexcept
    {
        if (this != &o)
        {
            _release();
            _take(o);
        }
        return *this;
    }

    /// Inline and branch only: most values never spill, and a dying value needs no state written back.
    CC_FORCE_INLINE ~bigfloat_sbo()
    {
        if (_cap_class != 0) [[unlikely]]
            _free_heap();
    }

    // observers

    [[nodiscard]] bool is_zero() const { return _size == 0; }
    [[nodiscard]] bool is_negative() const { return _negative != 0; }
    /// -1, 0 or +1: a predicate's answer.
    [[nodiscard]] int sign() const { return _size == 0 ? 0 : (_negative != 0 ? -1 : 1); }
    /// The weight of the lowest limb's bit 0; 0 for zero.
    [[nodiscard]] i32 exponent() const { return _exponent; }
    /// The mantissa, least significant limb first; its lowest and highest limbs are nonzero.
    [[nodiscard]] cc::span<u64 const> limbs() const { return cc::span<u64 const>(_data(), isize(_size)); }
    [[nodiscard]] bool is_inline() const { return _cap_class == 0; }
    /// The limbs held without reallocating; a value never moves back inline once it spilled.
    [[nodiscard]] i32 capacity() const { return _cap_class == 0 ? K : impl::bigfloat_class_limbs(_cap_class); }
    /// The magnitude, as the kernels read it.
    [[nodiscard]] impl::bigfloat_view view() const
    {
        return {.limbs = _data(), .size = i32(_size), .exponent = _exponent};
    }

    /// Rounded to nearest, ties to even; correctly unless the result is subnormal, and exactly when it is a double.
    [[nodiscard]] f64 to_f64() const { return impl::bigfloat_to_f64(view(), _negative != 0); }

    // arithmetic

    void negate()
    {
        if (_size != 0)
            _negative = !_negative;
    }

    // The small cases are forced inline into the caller and the general ones kept out of line:
    // a call costs a small operation as much as its arithmetic does, in saved registers alone.

    /// *this = a + b; a or b may be *this.
    CC_FORCE_INLINE void assign_add(bigfloat_sbo const& a, bigfloat_sbo const& b) { _assign_sum(a, b, false); }
    /// *this = a - b; a or b may be *this.
    CC_FORCE_INLINE void assign_sub(bigfloat_sbo const& a, bigfloat_sbo const& b) { _assign_sum(a, b, true); }

    /// *this = a * b; a or b may be *this.
    CC_FORCE_INLINE void assign_mul(bigfloat_sbo const& a, bigfloat_sbo const& b)
    {
        if (a._size - 1u < 2u && b._size - 1u < 2u) [[likely]]
        {
            // into a local window first, so the operands may alias *this and a one-limb result does not spill at K = 1
            u64 w[4];
            auto const r = impl::bigfloat_mul_small(w, a.view(), b.view());
            _assign_window<4>(w, {.size = r.size, .exponent = r.exponent, .negative = a._negative != b._negative});
            return;
        }
        _assign_mul_general(a, b);
    }

    /// *this = the sum of the terms, in one accumulator with at most one allocation; a term may refer to *this.
    void assign_msum(cc::span<bigfloat_summand const> terms)
    {
        for (auto const& t : terms)
            if ((_size != 0 && t.a.limbs == _data()) || (t.has_b && _size != 0 && t.b.limbs == _data()))
            {
                bigfloat_sbo r;
                r.assign_msum(terms);
                *this = cc::move(r);
                return;
            }
        auto const n = impl::bigfloat_msum_limbs(terms);
        if (n == 0)
        {
            _set({});
            return;
        }
        _reserve_discarding(n);
        _set(impl::bigfloat_msum_kernel(_data(), n, terms));
    }

    /// The sum of the terms: tg::bigfloat::msum({{a, b}, {c, d, true}, {e}}) is a * b - c * d + e.
    [[nodiscard]] static bigfloat_sbo msum(cc::span<bigfloat_summand const> terms)
    {
        bigfloat_sbo r;
        r.assign_msum(terms);
        return r;
    }

    [[nodiscard]] CC_FORCE_INLINE friend bigfloat_sbo operator-(bigfloat_sbo a)
    {
        a.negate();
        return a;
    }
    [[nodiscard]] CC_FORCE_INLINE friend bigfloat_sbo operator+(bigfloat_sbo const& a, bigfloat_sbo const& b)
    {
        bigfloat_sbo r;
        r.assign_add(a, b);
        return r;
    }
    [[nodiscard]] CC_FORCE_INLINE friend bigfloat_sbo operator-(bigfloat_sbo const& a, bigfloat_sbo const& b)
    {
        bigfloat_sbo r;
        r.assign_sub(a, b);
        return r;
    }
    [[nodiscard]] CC_FORCE_INLINE friend bigfloat_sbo operator*(bigfloat_sbo const& a, bigfloat_sbo const& b)
    {
        bigfloat_sbo r;
        r.assign_mul(a, b);
        return r;
    }
    CC_FORCE_INLINE bigfloat_sbo& operator+=(bigfloat_sbo const& b)
    {
        assign_add(*this, b);
        return *this;
    }
    CC_FORCE_INLINE bigfloat_sbo& operator-=(bigfloat_sbo const& b)
    {
        assign_sub(*this, b);
        return *this;
    }
    CC_FORCE_INLINE bigfloat_sbo& operator*=(bigfloat_sbo const& b)
    {
        assign_mul(*this, b);
        return *this;
    }

    // comparison

    /// The sign of a - b, without computing it.
    [[nodiscard]] friend int compare(bigfloat_sbo const& a, bigfloat_sbo const& b)
    {
        auto const sa = a.sign();
        auto const sb = b.sign();
        if (sa != sb)
            return sa < sb ? -1 : 1;
        if (sa == 0)
            return 0;
        auto const m = impl::bigfloat_compare_kernel(a.view(), b.view());
        return sa > 0 ? m : -m;
    }
    [[nodiscard]] friend bool operator==(bigfloat_sbo const& a, bigfloat_sbo const& b) { return compare(a, b) == 0; }
    [[nodiscard]] friend std::strong_ordering operator<=>(bigfloat_sbo const& a, bigfloat_sbo const& b)
    {
        auto const c = compare(a, b);
        return c < 0 ? std::strong_ordering::less : c > 0 ? std::strong_ordering::greater : std::strong_ordering::equal;
    }

private:
    union
    {
        u64 _inline_limbs[K];
        u64* _heap_limbs;
    };
    i32 _exponent = 0;
    u32 _size : 26 = 0;
    /// 0 for the inline limbs, else the node class of _heap_limbs.
    u32 _cap_class : 5 = 0;
    u32 _negative : 1 = 0;

    [[nodiscard]] u64* _data() { return _cap_class == 0 ? _inline_limbs : _heap_limbs; }
    [[nodiscard]] u64 const* _data() const { return _cap_class == 0 ? _inline_limbs : _heap_limbs; }

    /// m * 2^e, with m's trailing zeros moved into the exponent, so a product of two doubles keeps to two limbs.
    void _set_word(u64 m, int e, bool negative)
    {
        if (m == 0)
            return;
        auto const tz = cc::count_trailing_zeroes(m);
        _inline_limbs[0] = m >> tz;
        _size = 1;
        _exponent = e + tz;
        _negative = negative;
    }

    void _set(impl::bigfloat_raw r)
    {
        _size = u32(r.size);
        _exponent = r.size == 0 ? 0 : r.exponent;
        _negative = r.size != 0 && r.negative;
    }

    /// Capacity for n limbs; the current limbs may be lost.
    void _reserve_discarding(i32 n)
    {
        if (n <= capacity())
            return;
        _release();
        u32 c = 0;
        _heap_limbs = impl::bigfloat_allocate(n, c);
        _cap_class = c;
    }

    CC_DONT_INLINE void _free_heap() { impl::bigfloat_free(_heap_limbs, _cap_class); }

    void _release()
    {
        if (_cap_class != 0)
            impl::bigfloat_free(_heap_limbs, _cap_class);
        _cap_class = 0;
    }

    /// Takes o's limbs and leaves o zero; *this holds no allocation.
    void _take(bigfloat_sbo& o)
    {
        if (o._cap_class != 0)
            _heap_limbs = o._heap_limbs;
        else
            for (auto i = 0; i < i32(o._size); ++i)
                _inline_limbs[i] = o._inline_limbs[i];
        _cap_class = o._cap_class;
        _size = o._size;
        _exponent = o._exponent;
        _negative = o._negative;
        o._cap_class = 0;
        o._size = 0;
        o._exponent = 0;
        o._negative = 0;
    }

    /// *this = r, its limbs in the window w of N limbs, which is not *this's storage.
    /// A result that fits inline is copied as a fixed min(N, K) limbs, past its size too, so the copy does not branch on it.
    template <int N>
    CC_FORCE_INLINE void _assign_window(u64 const* w, impl::bigfloat_raw r)
    {
        if (_cap_class == 0 && r.size <= K) [[likely]]
        {
            for (auto i = 0; i < (N < K ? N : K); ++i)
                _inline_limbs[i] = w[i];
        }
        else
        {
            _reserve_discarding(r.size);
            auto* const d = _data();
            for (auto i = 0; i < r.size; ++i)
                d[i] = w[i];
        }
        _set(r);
    }

    CC_FORCE_INLINE void _assign_sum(bigfloat_sbo const& a, bigfloat_sbo const& b, bool subtract)
    {
        auto const av = a.view();
        auto const bv = b.view();
        if (av.size != 0 && bv.size != 0 && impl::bigfloat_is_small_sum(av, bv)) [[likely]]
        {
            auto const magnitude_sub = (a._negative != 0) != ((b._negative != 0) != subtract);
            if ((av.size | bv.size) == 1)
            {
                u64 w[2];
                auto const r = impl::bigfloat_add_small<1>(w, av, bv, magnitude_sub);
                _assign_window<2>(
                    w, {.size = r.size, .exponent = r.exponent, .negative = (a._negative != 0) != r.negative});
                return;
            }
            u64 w[3];
            auto const r = impl::bigfloat_add_small<2>(w, av, bv, magnitude_sub);
            _assign_window<3>(w, {.size = r.size, .exponent = r.exponent, .negative = (a._negative != 0) != r.negative});
            return;
        }
        _assign_sum_general(a, b, subtract);
    }

    CC_DONT_INLINE void _assign_mul_general(bigfloat_sbo const& a, bigfloat_sbo const& b)
    {
        if (a._size == 0 || b._size == 0)
        {
            _set({});
            return;
        }
        if (this == &a || this == &b)
        {
            bigfloat_sbo t;
            t._assign_mul_general(a, b);
            *this = cc::move(t);
            return;
        }
        _reserve_discarding(i32(a._size + b._size));
        auto const r = impl::bigfloat_mul_kernel(_data(), a.view(), b.view());
        _set({.size = r.size, .exponent = r.exponent, .negative = a._negative != b._negative});
    }

    CC_DONT_INLINE void _assign_sum_general(bigfloat_sbo const& a, bigfloat_sbo const& b, bool subtract)
    {
        // self-assignment is the only aliasing these two cases meet, and copy assignment handles it
        if (b._size == 0)
        {
            *this = a;
            return;
        }
        if (a._size == 0)
        {
            *this = b;
            if (subtract)
                negate();
            return;
        }
        if (this == &a || this == &b)
        {
            bigfloat_sbo t;
            t._assign_sum_general(a, b, subtract);
            *this = cc::move(t);
            return;
        }
        auto const magnitude_sub = (a._negative != 0) != ((b._negative != 0) != subtract);
        _reserve_discarding(impl::bigfloat_sum_limbs(a.view(), b.view()));
        auto const r = impl::bigfloat_add_kernel(_data(), a.view(), b.view(), magnitude_sub);
        _set({.size = r.size, .exponent = r.exponent, .negative = (a._negative != 0) != r.negative});
    }
};

template <int K>
tg::bigfloat_summand::bigfloat_summand(bigfloat_sbo<K> const& x, bool negate)
  : a(x.view()), negative(negate != x.is_negative())
{
}

template <int K>
tg::bigfloat_summand::bigfloat_summand(bigfloat_sbo<K> const& x, bigfloat_sbo<K> const& y, bool negate)
  : a(x.view()), b(y.view()), has_b(true), negative(negate != (x.is_negative() != y.is_negative()))
{
}
