#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <typed-geometry/scalar/float_expansion/impl/kernels.hh>

#include <compare>
#include <type_traits>

// tg::float_expansion_sbo<K>: an exact sum of doubles, Shewchuk's floating-point expansion, for ring arithmetic that never rounds.
// libs/base/typed-geometry/docs/modules/scalar.md has the design, and libs/base/typed-geometry/cheat-sheet.md the surface.

/// Exact for + - * and comparison while every intermediate stays in range: no sum may overflow,
/// and no product of two components may fall below 2^-960 in magnitude, where its roundoff would reach the subnormals.
/// Out of range is a CC_ASSERT, never a silent wrong value where assertions are on; tg::bigfloat has no such limit.
/// K components live inline, and a longer expansion spills to cc's node allocator; sizeof is 8 * K + 8.
template <int K>
struct tg::float_expansion_sbo
{
    static_assert(K >= 1, "K = 0 would cost the 16 bytes of K = 1, since the pointer needs the word anyway");

    static constexpr int inline_components = K;

    float_expansion_sbo() = default;

    /// Exact; v must be finite.
    float_expansion_sbo(f64 v)
    {
        CC_ASSERT(v - v == 0.0, "a float_expansion is finite");
        if (v != 0.0)
        {
            _inline_components[0] = v;
            _size = 1;
        }
    }

    /// Exact: an integer of up to 64 bits is two components at most, its high and low halves.
    template <class I>
        requires(std::is_integral_v<I> && !std::is_same_v<I, bool>)
    float_expansion_sbo(I v)
    {
        if constexpr (sizeof(I) <= 4)
            *this = float_expansion_sbo(f64(v));
        else
        {
            auto const negative = std::is_signed_v<I> && v < 0;
            u64 const m = negative ? u64(0) - u64(v) : u64(v);
            f64 h[2];
            _assign_window<2>(h, impl::float_expansion_from_limbs(&m, 1, negative, h));
        }
    }

    /// x, exactly; its magnitude must stay below 2^1024.
    template <int Bits, bool Signed>
        requires(Bits % 64 == 0)
    [[nodiscard]] static float_expansion_sbo from_fixed_int(impl::fixed_integer<Bits, Signed> const& x)
    {
        constexpr int n = Bits / 64;
        auto negative = false;
        if constexpr (Signed)
            negative = x.is_negative();
        // the magnitude of a negative value is ~x + 1
        u64 m[n];
        auto const flip = u64(0) - u64(negative);
        auto carry = u64(negative);
        for (auto i = 0; i < n; ++i)
        {
            auto const s = (x.limbs[i] ^ flip) + carry;
            carry = u64(s < carry);
            m[i] = s;
        }
        f64 h[2 * n];
        float_expansion_sbo r;
        r._assign_window<2 * n>(h, impl::float_expansion_from_limbs(m, n, negative, h));
        return r;
    }

    /// The integer whose magnitude is the limbs, least significant first, exactly; it must stay below 2^1024.
    [[nodiscard]] static float_expansion_sbo from_limbs(cc::span<u64 const> limbs, bool negative)
    {
        float_expansion_sbo r;
        auto const n = i32(limbs.size());
        r._reserve_discarding(2 * n);
        r._size = u32(impl::float_expansion_from_limbs(limbs.data(), n, negative, r._data()));
        return r;
    }

    float_expansion_sbo(float_expansion_sbo const& o) : _size(o._size)
    {
        auto* d = _inline_components;
        if (o._size > K)
        {
            u32 c = 0;
            _heap_components = impl::float_expansion_allocate(i32(o._size), c);
            _cap_class = c;
            d = _heap_components;
        }
        auto const* const s = o._data();
        for (auto i = 0; i < i32(o._size); ++i)
            d[i] = s[i];
    }

    float_expansion_sbo(float_expansion_sbo&& o) noexcept { _take(o); }

    float_expansion_sbo& operator=(float_expansion_sbo const& o)
    {
        if (this != &o)
        {
            _reserve_discarding(i32(o._size));
            auto* const d = _data();
            auto const* const s = o._data();
            for (auto i = 0; i < i32(o._size); ++i)
                d[i] = s[i];
            _size = o._size;
        }
        return *this;
    }

    float_expansion_sbo& operator=(float_expansion_sbo&& o) noexcept
    {
        if (this != &o)
        {
            _release();
            _take(o);
        }
        return *this;
    }

    /// Inline and branch only: most values never spill, and a dying value needs no state written back.
    CC_FORCE_INLINE ~float_expansion_sbo()
    {
        if (_cap_class != 0) [[unlikely]]
            _free_heap();
    }

    // observers

    [[nodiscard]] bool is_zero() const { return _size == 0; }
    /// -1, 0 or +1: a predicate's answer, the sign of the largest component.
    [[nodiscard]] int sign() const
    {
        if (_size == 0)
            return 0;
        return _data()[_size - 1] > 0.0 ? 1 : -1;
    }
    [[nodiscard]] bool is_negative() const { return sign() < 0; }
    /// The components, smallest magnitude first; none is zero.
    [[nodiscard]] cc::span<f64 const> components() const { return cc::span<f64 const>(_data(), isize(_size)); }
    [[nodiscard]] bool is_inline() const { return _cap_class == 0; }
    /// The components held without reallocating; a value never moves back inline once it spilled.
    [[nodiscard]] i32 capacity() const
    {
        return _cap_class == 0 ? K : impl::float_expansion_class_components(_cap_class);
    }

    /// Approximately, within a few ulps: the components summed from the smallest up.
    [[nodiscard]] f64 approximate() const { return impl::float_expansion_approximate(_data(), i32(_size)); }

    // arithmetic

    void negate()
    {
        auto* const d = _data();
        for (auto i = 0; i < i32(_size); ++i)
            d[i] = -d[i];
    }

    // A sum or product of single components, a difference of doubles or its product, is forced inline;
    // the general merges are one call.

    /// *this = a + b; a or b may be *this.
    CC_FORCE_INLINE void assign_add(float_expansion_sbo const& a, float_expansion_sbo const& b)
    {
        _assign_sum(a, b, false);
    }
    /// *this = a - b; a or b may be *this.
    CC_FORCE_INLINE void assign_sub(float_expansion_sbo const& a, float_expansion_sbo const& b)
    {
        _assign_sum(a, b, true);
    }

    /// *this = a * b; a or b may be *this.
    CC_FORCE_INLINE void assign_mul(float_expansion_sbo const& a, float_expansion_sbo const& b)
    {
        if (a._size == 1 && b._size == 1) [[likely]]
        {
            f64 h[2];
            _assign_window<2>(h, impl::float_expansion_two_product(a._data()[0], b._data()[0], h));
            return;
        }
        _assign_mul_general(a, b);
    }

    [[nodiscard]] CC_FORCE_INLINE friend float_expansion_sbo operator-(float_expansion_sbo a)
    {
        a.negate();
        return a;
    }
    [[nodiscard]] CC_FORCE_INLINE friend float_expansion_sbo operator+(float_expansion_sbo const& a,
                                                                       float_expansion_sbo const& b)
    {
        float_expansion_sbo r;
        r.assign_add(a, b);
        return r;
    }
    [[nodiscard]] CC_FORCE_INLINE friend float_expansion_sbo operator-(float_expansion_sbo const& a,
                                                                       float_expansion_sbo const& b)
    {
        float_expansion_sbo r;
        r.assign_sub(a, b);
        return r;
    }
    [[nodiscard]] CC_FORCE_INLINE friend float_expansion_sbo operator*(float_expansion_sbo const& a,
                                                                       float_expansion_sbo const& b)
    {
        float_expansion_sbo r;
        r.assign_mul(a, b);
        return r;
    }
    CC_FORCE_INLINE float_expansion_sbo& operator+=(float_expansion_sbo const& b)
    {
        assign_add(*this, b);
        return *this;
    }
    CC_FORCE_INLINE float_expansion_sbo& operator-=(float_expansion_sbo const& b)
    {
        assign_sub(*this, b);
        return *this;
    }
    CC_FORCE_INLINE float_expansion_sbo& operator*=(float_expansion_sbo const& b)
    {
        assign_mul(*this, b);
        return *this;
    }

    // comparison

    /// The sign of a - b, computed as the difference.
    [[nodiscard]] friend int compare(float_expansion_sbo const& a, float_expansion_sbo const& b)
    {
        auto const sa = a.sign();
        auto const sb = b.sign();
        if (sa != sb)
            return sa < sb ? -1 : 1;
        if (sa == 0)
            return 0;
        return (a - b).sign();
    }
    [[nodiscard]] friend bool operator==(float_expansion_sbo const& a, float_expansion_sbo const& b)
    {
        return compare(a, b) == 0;
    }
    [[nodiscard]] friend std::strong_ordering operator<=>(float_expansion_sbo const& a, float_expansion_sbo const& b)
    {
        auto const c = compare(a, b);
        return c < 0 ? std::strong_ordering::less : c > 0 ? std::strong_ordering::greater : std::strong_ordering::equal;
    }

private:
    union
    {
        f64 _inline_components[K];
        f64* _heap_components;
    };
    u32 _size = 0;
    /// 0 for the inline components, else the node class of _heap_components.
    u32 _cap_class = 0;

    [[nodiscard]] f64* _data() { return _cap_class == 0 ? _inline_components : _heap_components; }
    [[nodiscard]] f64 const* _data() const { return _cap_class == 0 ? _inline_components : _heap_components; }

    /// Capacity for n components; the current ones may be lost.
    void _reserve_discarding(i32 n)
    {
        if (n <= capacity())
            return;
        _release();
        u32 c = 0;
        _heap_components = impl::float_expansion_allocate(n, c);
        _cap_class = c;
    }

    CC_DONT_INLINE void _free_heap() { impl::float_expansion_free(_heap_components, _cap_class); }

    void _release()
    {
        if (_cap_class != 0)
            impl::float_expansion_free(_heap_components, _cap_class);
        _cap_class = 0;
    }

    /// Takes o's components and leaves o zero; *this holds no allocation.
    void _take(float_expansion_sbo& o)
    {
        if (o._cap_class != 0)
            _heap_components = o._heap_components;
        else
            for (auto i = 0; i < i32(o._size); ++i)
                _inline_components[i] = o._inline_components[i];
        _cap_class = o._cap_class;
        _size = o._size;
        o._cap_class = 0;
        o._size = 0;
    }

    /// *this = the n components in the window h of N, which is not *this's storage.
    template <int N>
    CC_FORCE_INLINE void _assign_window(f64 const* h, i32 n)
    {
        if (_cap_class == 0 && n <= K) [[likely]]
        {
            for (auto i = 0; i < (N < K ? N : K); ++i)
                _inline_components[i] = h[i];
        }
        else
        {
            _reserve_discarding(n);
            auto* const d = _data();
            for (auto i = 0; i < n; ++i)
                d[i] = h[i];
        }
        _size = u32(n);
    }

    CC_FORCE_INLINE void _assign_sum(float_expansion_sbo const& a, float_expansion_sbo const& b, bool subtract)
    {
        if (a._size == 1 && b._size == 1) [[likely]]
        {
            f64 h[2];
            _assign_window<2>(h, impl::float_expansion_two_sum(a._data()[0], subtract ? -b._data()[0] : b._data()[0], h));
            return;
        }
        _assign_sum_general(a, b, subtract);
    }

    CC_DONT_INLINE void _assign_sum_general(float_expansion_sbo const& a, float_expansion_sbo const& b, bool subtract)
    {
        if (this == &a || this == &b)
        {
            float_expansion_sbo t;
            t._assign_sum_general(a, b, subtract);
            *this = cc::move(t);
            return;
        }
        _reserve_discarding(i32(a._size + b._size));
        _size = u32(impl::float_expansion_sum(a._data(), i32(a._size), b._data(), i32(b._size), subtract, _data()));
    }

    CC_DONT_INLINE void _assign_mul_general(float_expansion_sbo const& a, float_expansion_sbo const& b)
    {
        if (a._size == 0 || b._size == 0)
        {
            _size = 0;
            return;
        }
        if (this == &a || this == &b)
        {
            float_expansion_sbo t;
            t._assign_mul_general(a, b);
            *this = cc::move(t);
            return;
        }
        _reserve_discarding(i32(2 * a._size * b._size));
        _size = u32(impl::float_expansion_product(a._data(), i32(a._size), b._data(), i32(b._size), _data()));
    }
};
