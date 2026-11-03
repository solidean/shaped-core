#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/memory/node_allocation.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <typed-geometry/scalar/fixed_int/fixed_arith.hh>
#include <typed-geometry/scalar/impl/limbs.hh>

#include <compare>
#include <type_traits>

// tg::bigint: an arbitrary-precision integer, sign and magnitude, one pointer wide.
// libs/base/typed-geometry/docs/modules/scalar.md has the design, and libs/base/typed-geometry/cheat-sheet.md the surface.

namespace tg::impl
{
/// The word in front of a bigint's limbs, at the start of its node.
struct bigint_header
{
    /// The limbs in use; the top one is nonzero, and size 0 is zero.
    u32 size = 0;
    /// The node class the node was allocated from, which frees it and sets its capacity.
    u8 node_class = 0;
    /// Never set on zero.
    bool negative = false;
    /// The limbs the node holds, saturated at 65535, so that a lean path's room check is one compare.
    /// Past that a node is still sized by its class, and only the lean paths, which test against this, step aside.
    u16 capacity = 0;
};
static_assert(sizeof(bigint_header) == 8);

[[nodiscard]] CC_FORCE_INLINE u64* bigint_limbs(bigint_header* h)
{
    return reinterpret_cast<u64*>(h + 1); // NOLINT
}
[[nodiscard]] CC_FORCE_INLINE u64 const* bigint_limbs(bigint_header const* h)
{
    return reinterpret_cast<u64 const*>(h + 1); // NOLINT
}

/// The limbs a node of the class holds behind its header: 1, 3, 7, 15, 31 for 16 to 256 bytes.
[[nodiscard]] constexpr i32 bigint_class_capacity(u32 node_class)
{
    return i32((u64(1) << node_class) / 8 - 1);
}

/// A node from cc's node allocator with room for at least `limbs` limbs, holding zero.
[[nodiscard]] CC_FORCE_INLINE bigint_header* bigint_allocate(i32 limbs)
{
    auto const idx = cc::node_class_index_from_size_and_align(isize(limbs + 1) * 8, 8);
    auto* const h = reinterpret_cast<bigint_header*>( // NOLINT
        cc::default_node_allocator().allocate_node_bytes(idx, isize(1) << int(idx), 8));
    // saturated by wrapping: from the 512 KiB class on, the limb count is a multiple of 2^16, so its u16 is 0, less one 65535
    *h = {.node_class = u8(idx), .capacity = u16(u16((u64(1) << int(idx)) / 8) - 1)};
    return h;
}

CC_FORCE_INLINE void bigint_free(bigint_header* h)
{
    cc::node_allocation_free(reinterpret_cast<byte*>(h), cc::node_class_index(h->node_class)); // NOLINT
}

/// bigint_allocate and bigint_free behind a call, for the paths an operation rarely takes.
[[nodiscard]] bigint_header* bigint_allocate_out_of_line(i32 limbs);
void bigint_free_out_of_line(bigint_header* h);
} // namespace tg::impl

/// Exact for + - * / % and shifts: the magnitude grows as the value needs, and nothing wraps.
/// One pointer wide: null is zero, and any other value is a node of cc's node allocator, a header word and then the limbs.
/// The node is sized by the allocator's power-of-two classes, so a value keeps spare limbs, and an assignment into it reuses them.
/// Division truncates toward zero, like the builtins; >> rounds toward negative infinity, like an arithmetic shift.
struct tg::bigint
{
    bigint() = default;

    template <class I>
        requires(std::is_integral_v<I> && !std::is_same_v<I, bool>)
    bigint(I v)
    {
        if constexpr (std::is_signed_v<I>)
            _set_word(v < 0 ? u64(0) - u64(v) : u64(v), v < 0);
        else
            _set_word(u64(v), false);
    }

    /// x exactly, read as two's complement when signed.
    template <int Bits, bool Signed>
        requires(Bits % 64 == 0)
    [[nodiscard]] static bigint from_fixed_int(impl::fixed_integer<Bits, Signed> const& x)
    {
        constexpr int n = Bits / 64;
        auto negative = false;
        if constexpr (Signed)
            negative = x.is_negative();
        bigint r;
        r._node = impl::bigint_allocate(n);
        auto* const d = impl::bigint_limbs(r._node);
        // the magnitude of a negative value is ~x + 1
        auto const flip = u64(0) - u64(negative);
        auto carry = u64(negative);
        for (auto i = 0; i < n; ++i)
        {
            auto const s = cc::add_with_carry(x.limbs[i] ^ flip, 0, carry);
            d[i] = s.value;
            carry = s.carry;
        }
        r._set(impl::limbs_normalized_size(d, n), negative);
        return r;
    }

    /// The magnitude's limbs, least significant first, zero limbs at the top allowed.
    [[nodiscard]] static bigint from_limbs(cc::span<u64 const> magnitude, bool negative = false);

    /// A decimal integer with an optional sign, digits only; nullopt for anything else.
    [[nodiscard]] static cc::optional<bigint> parse(cc::string_view decimal);

    bigint(bigint const& o)
    {
        if (!o.is_zero())
            _copy_new(o);
    }

    bigint(bigint&& o) noexcept : _node(cc::exchange(o._node, nullptr)) {}

    bigint& operator=(bigint const& o)
    {
        if (this != &o)
            _assign_copy(o);
        return *this;
    }

    bigint& operator=(bigint&& o) noexcept
    {
        if (this != &o)
        {
            if (_node != nullptr)
                impl::bigint_free(_node);
            _node = cc::exchange(o._node, nullptr);
        }
        return *this;
    }

    CC_FORCE_INLINE ~bigint()
    {
        if (_node != nullptr)
            impl::bigint_free(_node);
    }

    // observers

    [[nodiscard]] bool is_zero() const { return _node == nullptr || _node->size == 0; }
    [[nodiscard]] bool is_negative() const { return _node != nullptr && _node->negative; }
    /// -1, 0 or +1: a predicate's answer.
    [[nodiscard]] int sign() const { return is_zero() ? 0 : (_node->negative ? -1 : 1); }
    [[nodiscard]] bool is_odd() const { return !is_zero() && (impl::bigint_limbs(_node)[0] & 1) != 0; }
    /// The limbs of the magnitude.
    [[nodiscard]] i32 size() const { return _node == nullptr ? 0 : i32(_node->size); }
    /// The limbs held without reallocating; 0 when no node is held.
    [[nodiscard]] i32 capacity() const { return _node == nullptr ? 0 : impl::bigint_class_capacity(_node->node_class); }
    /// The magnitude, least significant limb first; its top limb is nonzero.
    [[nodiscard]] cc::span<u64 const> limbs() const
    {
        return _node == nullptr ? cc::span<u64 const>()
                                : cc::span<u64 const>(impl::bigint_limbs(_node), isize(_node->size));
    }
    /// The bits of the magnitude; 0 for zero.
    [[nodiscard]] i64 bit_width() const
    {
        auto const n = size();
        return n == 0 ? 0 : 64 * i64(n - 1) + cc::bit_width(impl::bigint_limbs(_node)[n - 1]);
    }

    [[nodiscard]] bool fits_i64() const;
    /// The value, which must fit.
    [[nodiscard]] i64 to_i64() const;
    /// Rounded to nearest, ties to even, and infinite past the double range.
    [[nodiscard]] f64 to_f64() const;
    /// In decimal, a minus sign in front of a negative value.
    [[nodiscard]] cc::string to_string() const;

    // arithmetic
    // Every assign_* reuses *this's node when it holds the result, and an operand may be *this.

    void negate()
    {
        if (!is_zero())
            _node->negative = !_node->negative;
    }

    // Operands of one or two limbs are forced inline, and everything else is one call:
    // at that size the call and its register saves cost more than the arithmetic.

    CC_FORCE_INLINE void assign_add(bigint const& a, bigint const& b)
    {
        if (!_try_sum_small(a, b, false)) [[unlikely]]
            if (!_try_sum(a, b, false)) [[unlikely]]
                _assign_sum_slow(a, b, false);
    }
    CC_FORCE_INLINE void assign_sub(bigint const& a, bigint const& b)
    {
        if (!_try_sum_small(a, b, true)) [[unlikely]]
            if (!_try_sum(a, b, true)) [[unlikely]]
                _assign_sum_slow(a, b, true);
    }
    CC_FORCE_INLINE void assign_mul(bigint const& a, bigint const& b)
    {
        if (!_try_mul_small(a, b)) [[unlikely]]
            _assign_mul(a, b);
    }
    /// *this = a << n, n >= 0.
    void assign_shl(bigint const& a, i64 n);
    /// *this = floor(a / 2^n), n >= 0.
    void assign_shr(bigint const& a, i64 n);

    /// a / b truncated toward zero, and a - b * quotient, which has a's sign; b != 0.
    friend div_mod_result<bigint> div_mod(bigint const& a, bigint const& b);
    /// The same into q and r, reusing their nodes as an assign_* does; q and r are two values, and either may be a or b.
    friend void div_mod(bigint& q, bigint& r, bigint const& a, bigint const& b);
    /// The greatest common divisor of |a| and |b|; gcd(0, 0) is 0.
    friend bigint gcd(bigint const& a, bigint const& b);
    [[nodiscard]] friend bigint abs(bigint a)
    {
        if (!a.is_zero())
            a._node->negative = false;
        return a;
    }

    [[nodiscard]] friend bigint operator-(bigint a)
    {
        a.negate();
        return a;
    }

    // An rvalue operand lends its node to the result, so a chain like a * b - c * d allocates once per product.
    [[nodiscard]] friend bigint operator+(bigint const& a, bigint const& b)
    {
        bigint r;
        r.assign_add(a, b);
        return r;
    }
    [[nodiscard]] friend bigint operator+(bigint&& a, bigint const& b)
    {
        a.assign_add(a, b);
        return cc::move(a);
    }
    [[nodiscard]] friend bigint operator+(bigint const& a, bigint&& b)
    {
        b.assign_add(a, b);
        return cc::move(b);
    }
    [[nodiscard]] friend bigint operator+(bigint&& a, bigint&& b)
    {
        a.assign_add(a, b);
        return cc::move(a);
    }
    [[nodiscard]] friend bigint operator-(bigint const& a, bigint const& b)
    {
        bigint r;
        r.assign_sub(a, b);
        return r;
    }
    [[nodiscard]] friend bigint operator-(bigint&& a, bigint const& b)
    {
        a.assign_sub(a, b);
        return cc::move(a);
    }
    [[nodiscard]] friend bigint operator-(bigint const& a, bigint&& b)
    {
        b.assign_sub(a, b);
        return cc::move(b);
    }
    [[nodiscard]] friend bigint operator-(bigint&& a, bigint&& b)
    {
        a.assign_sub(a, b);
        return cc::move(a);
    }
    [[nodiscard]] friend bigint operator*(bigint const& a, bigint const& b)
    {
        bigint r;
        r.assign_mul(a, b);
        return r;
    }
    [[nodiscard]] friend bigint operator*(bigint&& a, bigint const& b)
    {
        a.assign_mul(a, b);
        return cc::move(a);
    }
    [[nodiscard]] friend bigint operator*(bigint const& a, bigint&& b)
    {
        b.assign_mul(a, b);
        return cc::move(b);
    }
    [[nodiscard]] friend bigint operator*(bigint&& a, bigint&& b)
    {
        a.assign_mul(a, b);
        return cc::move(a);
    }
    [[nodiscard]] friend bigint operator/(bigint const& a, bigint const& b) { return div_mod(a, b).quotient; }
    [[nodiscard]] friend bigint operator%(bigint const& a, bigint const& b) { return div_mod(a, b).remainder; }
    [[nodiscard]] friend bigint operator<<(bigint const& a, i64 n)
    {
        bigint r;
        r.assign_shl(a, n);
        return r;
    }
    [[nodiscard]] friend bigint operator>>(bigint const& a, i64 n)
    {
        bigint r;
        r.assign_shr(a, n);
        return r;
    }

    bigint& operator+=(bigint const& b)
    {
        assign_add(*this, b);
        return *this;
    }
    bigint& operator-=(bigint const& b)
    {
        assign_sub(*this, b);
        return *this;
    }
    bigint& operator*=(bigint const& b)
    {
        assign_mul(*this, b);
        return *this;
    }
    bigint& operator/=(bigint const& b)
    {
        *this = div_mod(*this, b).quotient;
        return *this;
    }
    bigint& operator%=(bigint const& b)
    {
        *this = div_mod(*this, b).remainder;
        return *this;
    }
    bigint& operator<<=(i64 n)
    {
        assign_shl(*this, n);
        return *this;
    }
    bigint& operator>>=(i64 n)
    {
        assign_shr(*this, n);
        return *this;
    }

    // comparison

    /// The sign of a - b, without computing it.
    [[nodiscard]] friend int compare(bigint const& a, bigint const& b)
    {
        auto const sa = a.sign();
        auto const sb = b.sign();
        if (sa != sb)
            return sa < sb ? -1 : 1;
        if (sa == 0)
            return 0;
        auto const m = impl::limbs_cmp(impl::bigint_limbs(a._node), i32(a._node->size), impl::bigint_limbs(b._node),
                                       i32(b._node->size));
        return sa > 0 ? m : -m;
    }
    [[nodiscard]] friend bool operator==(bigint const& a, bigint const& b) { return compare(a, b) == 0; }
    [[nodiscard]] friend std::strong_ordering operator<=>(bigint const& a, bigint const& b)
    {
        auto const c = compare(a, b);
        return c < 0 ? std::strong_ordering::less : c > 0 ? std::strong_ordering::greater : std::strong_ordering::equal;
    }

private:
    impl::bigint_header* _node = nullptr;

    void _set(i32 size, bool negative)
    {
        _node->size = u32(size);
        _node->negative = size != 0 && negative;
    }

    void _set_word(u64 m, bool negative)
    {
        if (m == 0)
            return;
        _node = impl::bigint_allocate(1);
        impl::bigint_limbs(_node)[0] = m;
        _set(1, negative);
    }

    /// Whether a node holds one or two limbs, the sizes the inline paths take.
    [[nodiscard]] CC_FORCE_INLINE static bool _is_small(impl::bigint_header const* h)
    {
        return h != nullptr && h->size - 1u < 2u;
    }

    /// The limb above the lowest of a small node, 0 for a one-limb one; a one-limb node may have no room above.
    [[nodiscard]] CC_FORCE_INLINE static u64 _high_of_small(impl::bigint_header const* h)
    {
        return h->size == 2 ? impl::bigint_limbs(h)[1] : u64(0);
    }

    /// Writes a result of up to four limbs into *this's node, a fresh one when it has none; false when the node is too small.
    /// The class decides the room: 16 bytes hold 1 limb, 32 hold 3, 64 and up hold 7 or more.
    /// A fixed prefix is written by class rather than by size, since the limbs past the size are free to hold anything.
    [[nodiscard]] CC_FORCE_INLINE bool _store_small(u64 r0, u64 r1, u64 r2, u64 r3, i32 size, bool negative)
    {
        auto* h = _node;
        if (h == nullptr) [[unlikely]]
            h = _node = impl::bigint_allocate_out_of_line(size < 1 ? 1 : size);
        auto const node_class = h->node_class;
        auto* const d = impl::bigint_limbs(h);
        if (node_class >= 6) [[likely]]
        {
            d[0] = r0;
            d[1] = r1;
            d[2] = r2;
            d[3] = r3;
        }
        else if (node_class == 5)
        {
            if (size > 3)
                return false;
            d[0] = r0;
            d[1] = r1;
            d[2] = r2;
        }
        else
        {
            if (size > 1)
                return false;
            d[0] = r0;
        }
        h->size = u32(size);
        h->negative = size != 0 && negative;
        return true;
    }

    /// a ± b for operands of one or two limbs; false leaves *this's value untouched.
    /// The operands are read before *this is written, so either may be *this.
    [[nodiscard]] CC_FORCE_INLINE bool _try_sum_small(bigint const& a, bigint const& b, bool subtract)
    {
        auto* const ah = a._node;
        auto* const bh = b._node;
        if (!_is_small(ah) || !_is_small(bh))
            return false;
        auto const x0 = impl::bigint_limbs(ah)[0];
        auto const y0 = impl::bigint_limbs(bh)[0];
        auto const a_negative = ah->negative;
        auto const b_negative = bh->negative != subtract;
        if ((ah->size | bh->size) == 1)
        {
            // one limb each, the most common case, without the second limb's reads and carries
            if (a_negative == b_negative)
            {
                auto const s = cc::add_with_carry(x0, y0);
                return _store_small(s.value, s.carry, 0, 0, 1 + i32(s.carry), a_negative);
            }
            auto const y_larger = x0 < y0;
            auto const m = y_larger ? y0 - x0 : x0 - y0;
            return _store_small(m, 0, 0, 0, i32(m != 0), y_larger ? b_negative : a_negative);
        }
        auto const x1 = _high_of_small(ah);
        auto const y1 = _high_of_small(bh);
        if (a_negative == b_negative)
        {
            auto const s0 = cc::add_with_carry(x0, y0);
            auto const s1 = cc::add_with_carry(x1, y1, s0.carry);
            auto const size = s1.carry != 0 ? 3 : (s1.value != 0 ? 2 : 1);
            return _store_small(s0.value, s1.value, s1.carry, 0, size, a_negative);
        }
        // x - y, negated back when it borrows: the larger magnitude's sign, and zero for equal ones
        auto const d0 = cc::sub_with_borrow(x0, y0);
        auto const d1 = cc::sub_with_borrow(x1, y1, d0.borrow);
        auto const mask = u64(0) - d1.borrow;
        auto const n0 = cc::add_with_carry(d0.value ^ mask, 0, d1.borrow);
        auto const r1 = (d1.value ^ mask) + n0.carry;
        auto const size = r1 != 0 ? 2 : (n0.value != 0 ? 1 : 0);
        return _store_small(n0.value, r1, 0, 0, size, d1.borrow != 0 ? b_negative : a_negative);
    }

    /// a * b for operands of one or two limbs; false leaves *this's value untouched.
    [[nodiscard]] CC_FORCE_INLINE bool _try_mul_small(bigint const& a, bigint const& b)
    {
        auto* const ah = a._node;
        auto* const bh = b._node;
        if (!_is_small(ah) || !_is_small(bh))
            return false;
        auto const negative = ah->negative != bh->negative;
        auto const x0 = impl::bigint_limbs(ah)[0];
        auto const y0 = impl::bigint_limbs(bh)[0];
        if ((ah->size | bh->size) == 1)
        {
            auto const p = cc::umul128(x0, y0);
            return _store_small(p.lo, p.hi, 0, 0, p.hi != 0 ? 2 : 1, negative);
        }
        auto const x1 = _high_of_small(ah);
        auto const y1 = _high_of_small(bh);
        auto const p00 = cc::umul128(x0, y0);
        auto const p01 = cc::umul128(x0, y1);
        auto const p10 = cc::umul128(x1, y0);
        auto const p11 = cc::umul128(x1, y1);
        auto const s1 = cc::add_with_carry(p00.hi, p01.lo);
        auto const t1 = cc::add_with_carry(s1.value, p10.lo);
        auto const s2 = cc::add_with_carry(p01.hi, p10.hi, s1.carry);
        auto const t2 = cc::add_with_carry(s2.value, p11.lo, t1.carry);
        auto const r3 = p11.hi + s2.carry + t2.carry;
        // one operand has two limbs, so the product reaches 2^64
        auto const size = r3 != 0 ? 4 : (t2.value != 0 ? 3 : 2);
        return _store_small(p00.lo, t1.value, t2.value, r3, size, negative);
    }

    void _copy_new(bigint const& o);
    void _assign_copy(bigint const& o);
    /// a ± b for nonzero operands into a node that holds the result; false leaves *this untouched, for _assign_sum_slow.
    /// It never calls the slow path itself, so a, b and subtract need not outlive its first decisions.
    [[nodiscard]] bool _try_sum(bigint const& a, bigint const& b, bool subtract);
    void _assign_mul(bigint const& a, bigint const& b);
    void _assign_mul_slow(bigint const& a, bigint const& b);
    void _assign_sum_slow(bigint const& a, bigint const& b, bool subtract);
    /// The sum of two terms of opposite signs, the first of ah's magnitude and sign, the second of bh's magnitude; false leaves *this untouched.
    [[nodiscard]] bool _try_difference(impl::bigint_header const* ah, impl::bigint_header const* bh);
    void _assign_sum_with_zero(bigint const& a, bigint const& b, bool subtract);
    void _assign_mul_aliased(u64 const* x, i32 xn, u64 const* y, i32 yn, bool negative);

    /// Room for n limbs: *this's own node when it holds them, else a fresh one.
    /// The replaced node is returned rather than freed, since an operand may still read from it; the caller frees it last.
    /// Forced inline, so `replaced` lives in a register rather than a stack slot.
    [[nodiscard]] CC_FORCE_INLINE u64* _room(i32 n, impl::bigint_header*& replaced)
    {
        replaced = nullptr;
        // the header's capacity is one compare; the class decides only past its saturation
        if (_node != nullptr && (i32(_node->capacity) >= n || impl::bigint_class_capacity(_node->node_class) >= n))
            return impl::bigint_limbs(_node);
        replaced = _node;
        _node = impl::bigint_allocate_out_of_line(n);
        return impl::bigint_limbs(_node);
    }
};

namespace tg
{
[[nodiscard]] div_mod_result<bigint> div_mod(bigint const& a, bigint const& b);
void div_mod(bigint& q, bigint& r, bigint const& a, bigint const& b);
[[nodiscard]] bigint gcd(bigint const& a, bigint const& b);
} // namespace tg
