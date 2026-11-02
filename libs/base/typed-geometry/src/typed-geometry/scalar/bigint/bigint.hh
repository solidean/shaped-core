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
    /// Unused; held back for a rational's flags.
    u16 reserved = 0;
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
    *h = {.node_class = u8(idx)};
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

    // One-limb operands into a node that holds the result are forced inline, and everything else is one call:
    // at that size the call and its register saves cost more than the arithmetic.

    CC_FORCE_INLINE void assign_add(bigint const& a, bigint const& b)
    {
        if (!_try_sum_1(a, b, false)) [[unlikely]]
            _assign_sum(a, b, false);
    }
    CC_FORCE_INLINE void assign_sub(bigint const& a, bigint const& b)
    {
        if (!_try_sum_1(a, b, true)) [[unlikely]]
            _assign_sum(a, b, true);
    }
    CC_FORCE_INLINE void assign_mul(bigint const& a, bigint const& b)
    {
        if (!_try_mul_1(a, b)) [[unlikely]]
            _assign_mul(a, b);
    }
    /// *this = a << n, n >= 0.
    void assign_shl(bigint const& a, i64 n);
    /// *this = floor(a / 2^n), n >= 0.
    void assign_shr(bigint const& a, i64 n);

    /// a / b truncated toward zero, and a - b * quotient, which has a's sign; b != 0.
    friend div_mod_result<bigint> div_mod(bigint const& a, bigint const& b);
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

    /// The node class from which a node holds two limbs: 32 bytes.
    static constexpr u8 _two_limb_class = 5;

    /// *this's node for a result of one-limb operands: its own, or a fresh one of two limbs, as the operators' results start without one.
    [[nodiscard]] CC_FORCE_INLINE impl::bigint_header* _node_for_1()
    {
        if (_node == nullptr)
            _node = impl::bigint_allocate_out_of_line(2);
        return _node;
    }

    /// a ± b for one-limb operands, when *this's node holds the result; false leaves *this's value untouched.
    /// The operands are read before *this is written, so either may be *this.
    [[nodiscard]] CC_FORCE_INLINE bool _try_sum_1(bigint const& a, bigint const& b, bool subtract)
    {
        auto* const ah = a._node;
        auto* const bh = b._node;
        if (ah == nullptr || bh == nullptr || ah->size != 1 || bh->size != 1)
            return false;
        auto* const h = _node_for_1();
        auto const x = impl::bigint_limbs(ah)[0];
        auto const y = impl::bigint_limbs(bh)[0];
        auto const a_negative = ah->negative;
        auto const b_negative = bh->negative != subtract;
        auto* const d = impl::bigint_limbs(h);
        if (a_negative == b_negative)
        {
            auto const s = cc::add_with_carry(x, y);
            if (s.carry != 0)
            {
                if (h->node_class < _two_limb_class)
                    return false;
                d[1] = 1;
            }
            d[0] = s.value;
            h->size = u32(1 + s.carry);
            h->negative = a_negative;
            return true;
        }
        // the larger magnitude's sign; equal magnitudes give zero
        auto const y_larger = x < y;
        auto const m = y_larger ? y - x : x - y;
        d[0] = m;
        h->size = u32(m != 0);
        h->negative = m != 0 && (y_larger ? b_negative : a_negative);
        return true;
    }

    /// a * b for one-limb operands, when *this's node holds two limbs or more; false leaves *this's value untouched.
    [[nodiscard]] CC_FORCE_INLINE bool _try_mul_1(bigint const& a, bigint const& b)
    {
        auto* const ah = a._node;
        auto* const bh = b._node;
        if (ah == nullptr || bh == nullptr || ah->size != 1 || bh->size != 1)
            return false;
        auto* const h = _node_for_1();
        if (h->node_class < _two_limb_class)
            return false;
        auto const p = cc::umul128(impl::bigint_limbs(ah)[0], impl::bigint_limbs(bh)[0]);
        auto const negative = ah->negative != bh->negative;
        auto* const d = impl::bigint_limbs(h);
        d[0] = p.lo;
        d[1] = p.hi;
        h->size = u32(1 + (p.hi != 0));
        h->negative = negative;
        return true;
    }

    void _copy_new(bigint const& o);
    void _assign_copy(bigint const& o);
    void _assign_sum(bigint const& a, bigint const& b, bool subtract);
    void _assign_mul(bigint const& a, bigint const& b);
    void _assign_sum_with_zero(bigint const& a, bigint const& b, bool subtract);
    void _assign_mul_aliased(u64 const* x, i32 xn, u64 const* y, i32 yn, bool negative);

    /// Room for n limbs: *this's own node when it holds them, else a fresh one.
    /// The replaced node is returned rather than freed, since an operand may still read from it; the caller frees it last.
    /// Forced inline, so `replaced` lives in a register rather than a stack slot.
    [[nodiscard]] CC_FORCE_INLINE u64* _room(i32 n, impl::bigint_header*& replaced)
    {
        replaced = nullptr;
        if (_node != nullptr && impl::bigint_class_capacity(_node->node_class) >= n)
            return impl::bigint_limbs(_node);
        replaced = _node;
        _node = impl::bigint_allocate_out_of_line(n);
        return impl::bigint_limbs(_node);
    }
};

namespace tg
{
[[nodiscard]] div_mod_result<bigint> div_mod(bigint const& a, bigint const& b);
[[nodiscard]] bigint gcd(bigint const& a, bigint const& b);
} // namespace tg
