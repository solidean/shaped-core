#include <clean-core/container/vector.hh>
#include <typed-geometry/scalar/bigfloat/impl/kernels.hh>
#include <typed-geometry/scalar/bigint/bigint.hh>

namespace
{
using namespace cc::primitive_defines;
using tg::impl::bigint_header;
using tg::impl::bigint_limbs;

/// A product or quotient of at most this many limbs is computed on the stack when it cannot go straight into its node.
constexpr i32 stack_limbs = 64;

/// 10^19, the largest power of ten in a limb.
constexpr u64 decimal_chunk = 10'000'000'000'000'000'000ull;
constexpr int decimal_chunk_digits = 19;

CC_FORCE_INLINE void free_if(bigint_header* h)
{
    if (h != nullptr) [[unlikely]]
        tg::impl::bigint_free_out_of_line(h);
}
} // namespace

// Kept out of line even here, where the compiler could inline them: the allocator inlined into an operation costs it registers.

CC_DONT_INLINE tg::impl::bigint_header* tg::impl::bigint_allocate_out_of_line(i32 limbs)
{
    return bigint_allocate(limbs);
}

CC_DONT_INLINE void tg::impl::bigint_free_out_of_line(bigint_header* h)
{
    bigint_free(h);
}

tg::bigint tg::bigint::from_limbs(cc::span<u64 const> magnitude, bool negative)
{
    auto const n = impl::limbs_normalized_size(magnitude.data(), i32(magnitude.size()));
    bigint r;
    if (n == 0)
        return r;
    r._node = impl::bigint_allocate(n);
    auto* const d = bigint_limbs(r._node);
    for (auto i = 0; i < n; ++i)
        d[i] = magnitude[i];
    r._set(n, negative);
    return r;
}

cc::optional<tg::bigint> tg::bigint::parse(cc::string_view decimal)
{
    auto i = isize(0);
    auto negative = false;
    if (i < decimal.size() && (decimal[i] == '-' || decimal[i] == '+'))
    {
        negative = decimal[i] == '-';
        ++i;
    }
    if (i == decimal.size())
        return {};

    // one limb per 19 digits holds the value, the carry of each multiply included
    auto limbs = cc::vector<u64>();
    limbs.resize_to_filled((decimal.size() - i) / decimal_chunk_digits + 2, u64(0));
    auto n = i32(0);
    while (i < decimal.size())
    {
        u64 chunk = 0;
        u64 scale = 1;
        for (auto k = 0; k < decimal_chunk_digits && i < decimal.size(); ++k, ++i)
        {
            auto const c = decimal[i];
            if (c < '0' || c > '9')
                return {};
            chunk = chunk * 10 + u64(c - '0');
            scale *= 10;
        }
        auto const top = impl::limbs_mul_1(limbs.data(), limbs.data(), n, scale);
        if (top != 0)
            limbs[n++] = top;
        if (n == 0)
            n = 1;
        auto const carry = impl::limbs_add(limbs.data(), limbs.data(), n, &chunk, 1);
        if (carry != 0)
            limbs[n++] = carry;
    }
    return from_limbs(cc::span<u64 const>(limbs.data(), isize(n)), negative);
}

bool tg::bigint::fits_i64() const
{
    auto const n = size();
    if (n == 0)
        return true;
    if (n > 1)
        return false;
    auto const m = bigint_limbs(_node)[0];
    return _node->negative ? m <= u64(1) << 63 : m < u64(1) << 63;
}

i64 tg::bigint::to_i64() const
{
    CC_ASSERT(fits_i64(), "the bigint does not fit an i64");
    if (is_zero())
        return 0;
    auto const m = bigint_limbs(_node)[0];
    return _node->negative ? i64(u64(0) - m) : i64(m);
}

f64 tg::bigint::to_f64() const
{
    return impl::bigfloat_to_f64(
        {.limbs = _node == nullptr ? nullptr : bigint_limbs(_node), .size = size(), .exponent = 0}, is_negative());
}

cc::string tg::bigint::to_string() const
{
    if (is_zero())
        return cc::string("0");

    // the magnitude divided down by 10^19, collecting the remainders, least significant first
    auto n = size();
    auto rest = cc::vector<u64>();
    rest.resize_to_filled(n, u64(0));
    auto const* const src = bigint_limbs(_node);
    for (auto i = 0; i < n; ++i)
        rest[i] = src[i];
    auto chunks = cc::vector<u64>();
    while (n > 0)
    {
        chunks.push_back(impl::limbs_divrem_1(rest.data(), rest.data(), n, decimal_chunk));
        n = impl::limbs_normalized_size(rest.data(), n);
    }

    auto s = cc::string();
    if (_node->negative)
        s.push_back('-');
    char digits[decimal_chunk_digits];
    for (auto c = chunks.size() - 1; c >= 0; --c)
    {
        auto v = chunks[c];
        auto k = decimal_chunk_digits;
        do
        {
            digits[--k] = char('0' + v % 10);
            v /= 10;
        } while (v != 0);
        // every chunk below the top one is padded to its 19 digits
        if (c != chunks.size() - 1)
            while (k > 0)
                digits[--k] = '0';
        for (; k < decimal_chunk_digits; ++k)
            s.push_back(digits[k]);
    }
    return s;
}

void tg::bigint::_copy_new(bigint const& o)
{
    auto const n = i32(o._node->size);
    _node = impl::bigint_allocate(n);
    auto* const d = bigint_limbs(_node);
    auto const* const s = bigint_limbs(o._node);
    for (auto i = 0; i < n; ++i)
        d[i] = s[i];
    _set(n, o._node->negative);
}

void tg::bigint::_assign_copy(bigint const& o)
{
    auto const n = o.size();
    if (n == 0)
    {
        if (_node != nullptr)
            _set(0, false);
        return;
    }
    bigint_header* replaced = nullptr;
    auto* const d = _room(n, replaced);
    free_if(replaced);
    auto const* const s = bigint_limbs(o._node);
    for (auto i = 0; i < n; ++i)
        d[i] = s[i];
    _set(n, o._node->negative);
}

void tg::bigint::_assign_sum_with_zero(bigint const& a, bigint const& b, bool subtract)
{
    if (b.is_zero())
    {
        if (this != &a)
            _assign_copy(a);
        return;
    }
    if (this != &b)
        _assign_copy(b);
    if (subtract)
        negate();
}

bool tg::bigint::_try_sum(bigint const& a, bigint const& b, bool subtract)
{
    // the common case, kept lean so it needs few registers: nonzero operands into a node that holds the result
    // the kernels read limb i before they write it, so *this may be either operand
    auto* const h = _node;
    auto const* xh = a._node;
    auto const* yh = b._node;
    // one branch on the lowest address: as three tests clang merges them into setcc chains it then tests
    if (cc::min(cc::min(reinterpret_cast<u64>(h), reinterpret_cast<u64>(xh)), reinterpret_cast<u64>(yh)) == 0)
        [[unlikely]] // NOLINT
        return false;
    // terms of opposite signs, b's being -b in a subtraction, go to the difference before anything else is live
    if ((xh->negative != yh->negative) != subtract)
        return _try_difference(xh, yh);
    // a sum of magnitudes, with the sign both terms share, x the longer one
    auto const negative = xh->negative;
    auto xn = i32(xh->size);
    auto yn = i32(yh->size);
    if (xn < yn)
    {
        cc::swap(xh, yh);
        cc::swap(xn, yn);
    }
    // room for a carry limb; a node exactly one short goes the slow way, which grows it
    if (yn == 0 || i32(h->capacity) <= xn) [[unlikely]]
        return false;
    h->size = u32(xn);
    h->negative = negative;
    if (impl::limbs_add(bigint_limbs(h), bigint_limbs(xh), xn, bigint_limbs(yh), yn) != 0) [[unlikely]]
    {
        // the size read back from the node, so that nothing but h lives across the kernel
        auto const n = h->size;
        bigint_limbs(h)[n] = 1;
        h->size = n + 1;
    }
    return true;
}

CC_DONT_INLINE bool tg::bigint::_try_difference(bigint_header const* ah, bigint_header const* bh)
{
    // the smaller magnitude from the larger, with the sign of the larger one's term: a's sign, or its opposite
    auto const* xh = ah;
    auto const* yh = bh;
    auto xn = i32(xh->size);
    auto yn = i32(yh->size);
    auto negative = ah->negative;
    if (xn < yn)
    {
        cc::swap(xh, yh);
        cc::swap(xn, yn);
        negative = !negative;
    }
    auto* const h = _node;
    if (yn == 0 || i32(h->capacity) < xn) [[unlikely]]
        return false;
    auto const* x = bigint_limbs(xh);
    auto const* y = bigint_limbs(yh);
    if (xn == yn)
    {
        auto const c = impl::limbs_cmp(x, y, xn);
        if (c == 0)
        {
            h->size = 0;
            h->negative = false;
            return true;
        }
        if (c < 0)
        {
            cc::swap(x, y);
            negative = !negative;
        }
    }
    auto* const out = bigint_limbs(h);
    (void)impl::limbs_sub(out, x, xn, y, yn);
    h->size = u32(impl::limbs_normalized_size(out, xn));
    h->negative = negative;
    return true;
}

CC_DONT_INLINE void tg::bigint::_assign_sum_slow(bigint const& a, bigint const& b, bool subtract)
{
    auto const an = a.size();
    auto const bn = b.size();
    if (an == 0 || bn == 0) [[unlikely]]
    {
        _assign_sum_with_zero(a, b, subtract);
        return;
    }

    auto const* const ap = bigint_limbs(a._node);
    auto const* const bp = bigint_limbs(b._node);
    auto const a_negative = a._node->negative;
    auto const b_negative = b._node->negative != subtract;
    bigint_header* replaced = nullptr;

    if (a_negative == b_negative)
    {
        // |a| + |b|, the longer one first
        auto const a_longer = an >= bn;
        auto const* const x = a_longer ? ap : bp;
        auto const* const y = a_longer ? bp : ap;
        auto const xn = a_longer ? an : bn;
        auto const yn = a_longer ? bn : an;
        // room for the carry even when this sum has none, so that the next sum into *this takes the lean path
        auto* const out = _room(xn + 1, replaced);
        auto const carry = impl::limbs_add(out, x, xn, y, yn);
        if (carry != 0)
            out[xn] = carry;
        _set(xn + i32(carry), a_negative);
    }
    else
    {
        // |a| - |b|: the smaller magnitude from the larger, with the larger one's sign
        auto const c = impl::limbs_cmp(ap, an, bp, bn);
        if (c == 0)
        {
            if (_node != nullptr)
                _set(0, false);
            return;
        }
        auto const* const x = c > 0 ? ap : bp;
        auto const* const y = c > 0 ? bp : ap;
        auto const xn = c > 0 ? an : bn;
        auto const yn = c > 0 ? bn : an;
        auto* const out = _room(xn, replaced);
        impl::limbs_sub(out, x, xn, y, yn);
        _set(impl::limbs_normalized_size(out, xn), c > 0 ? a_negative : b_negative);
    }
    free_if(replaced);
}

void tg::bigint::_assign_mul(bigint const& a, bigint const& b)
{
    // the common case, kept lean so it needs few registers: nonzero operands, and a target that is neither and holds the product,
    // or has no node yet, as an operator's result starts out
    auto* const ah = a._node;
    auto* const bh = b._node;
    auto* h = _node;
    if (ah != nullptr && bh != nullptr && h != ah && h != bh) [[likely]]
    {
        auto const an = i32(ah->size);
        auto const bn = i32(bh->size);
        auto const n = an + bn;
        if (h == nullptr && an != 0 && bn != 0)
            h = _node = impl::bigint_allocate_out_of_line(n);
        if (h != nullptr && an != 0 && bn != 0 && i32(h->capacity) >= n) [[likely]]
        {
            auto* const out = bigint_limbs(h);
            impl::limbs_mul(out, bigint_limbs(ah), an, bigint_limbs(bh), bn);
            h->size = u32(n - i32(out[n - 1] == 0));
            h->negative = ah->negative != bh->negative;
            return;
        }
    }
    _assign_mul_slow(a, b);
}

CC_DONT_INLINE void tg::bigint::_assign_mul_slow(bigint const& a, bigint const& b)
{
    auto const an = a.size();
    auto const bn = b.size();
    if (an == 0 || bn == 0)
    {
        if (_node != nullptr)
            _set(0, false);
        return;
    }

    auto const negative = a._node->negative != b._node->negative;
    // the longer operand is the inner loop
    auto const a_longer = an >= bn;
    auto const* const x = bigint_limbs(a_longer ? a._node : b._node);
    auto const* const y = bigint_limbs(a_longer ? b._node : a._node);
    auto const xn = a_longer ? an : bn;
    auto const yn = a_longer ? bn : an;
    bigint_header* replaced = nullptr;

    if (yn == 1)
    {
        // one row, read before it is written, so *this may be either operand
        auto const d = y[0];
        auto* const out = _room(xn + 1, replaced);
        auto const top = impl::limbs_mul_1(out, x, xn, d);
        out[xn] = top;
        _set(xn + i32(top != 0), negative);
        free_if(replaced);
        return;
    }

    auto const n = xn + yn;
    if (this != &a && this != &b)
    {
        auto* const out = _room(n, replaced);
        impl::limbs_mul(out, x, xn, y, yn);
        _set(n - i32(out[n - 1] == 0), negative);
        free_if(replaced);
        return;
    }

    _assign_mul_aliased(x, xn, y, yn, negative);
}

void tg::bigint::_assign_mul_aliased(u64 const* x, i32 xn, u64 const* y, i32 yn, bool negative)
{
    // *this is an operand, so the product goes elsewhere first
    auto const n = xn + yn;
    if (n <= stack_limbs && capacity() >= n)
    {
        u64 tmp[stack_limbs];
        impl::limbs_mul(tmp, x, xn, y, yn);
        auto* const out = bigint_limbs(_node);
        for (auto i = 0; i < n; ++i)
            out[i] = tmp[i];
        _set(n - i32(tmp[n - 1] == 0), negative);
        return;
    }
    auto* const fresh = impl::bigint_allocate(n);
    auto* const out = bigint_limbs(fresh);
    impl::limbs_mul(out, x, xn, y, yn);
    impl::bigint_free(_node);
    _node = fresh;
    _set(n - i32(out[n - 1] == 0), negative);
}

void tg::bigint::assign_shl(bigint const& a, i64 n)
{
    CC_ASSERT(n >= 0, "a shift count is nonnegative");
    auto const an = a.size();
    if (an == 0)
    {
        if (_node != nullptr)
            _set(0, false);
        return;
    }
    CC_ASSERT(n < (i64(1) << 36), "a shifted bigint holds fewer than 2^30 limbs");
    auto const w = i32(n >> 6);
    auto const s = int(n & 63);
    auto const negative = a._node->negative;
    auto const* const ap = bigint_limbs(a._node);
    // the room is exact, so a power-of-two size lands in its own class
    auto const spills = s != 0 && (ap[an - 1] >> (64 - s)) != 0;
    bigint_header* replaced = nullptr;
    auto* const out = _room(an + w + i32(spills), replaced);
    // from the top down, so an in-place shift reads each limb before it is overwritten
    if (s == 0)
        for (auto i = an - 1; i >= 0; --i)
            out[i + w] = ap[i];
    else
    {
        auto const top = impl::limbs_lshift(out + w, ap, an, s);
        if (spills)
            out[an + w] = top;
    }
    for (auto i = 0; i < w; ++i)
        out[i] = 0;
    _set(an + w + i32(spills), negative);
    free_if(replaced);
}

void tg::bigint::assign_shr(bigint const& a, i64 n)
{
    CC_ASSERT(n >= 0, "a shift count is nonnegative");
    auto const an = a.size();
    auto const negative = a.is_negative();
    if (an == 0 || (n >> 6) >= an)
    {
        // every bit is shifted out: floor gives 0, or -1 for a negative value
        if (negative)
            *this = bigint(-1);
        else if (_node != nullptr)
            _set(0, false);
        return;
    }
    auto const w = i32(n >> 6);
    auto const s = int(n & 63);
    auto const* const ap = bigint_limbs(a._node);
    // rounding toward negative infinity takes a negative value's magnitude up when a set bit is shifted out
    auto lost = false;
    if (negative)
    {
        for (auto i = 0; i < w && !lost; ++i)
            lost = ap[i] != 0;
        lost = lost || (s != 0 && (ap[w] << (64 - s)) != 0);
    }
    auto const rn = an - w;
    bigint_header* replaced = nullptr;
    auto* const out = _room(rn + i32(lost), replaced);
    // from the bottom up, so an in-place shift reads each limb before it is overwritten
    if (s == 0)
        for (auto i = 0; i < rn; ++i)
            out[i] = ap[i + w];
    else
        (void)impl::limbs_rshift(out, ap + w, rn, s);
    auto size = impl::limbs_normalized_size(out, rn);
    if (lost)
    {
        u64 const one = 1;
        auto const carry = size == 0 ? (out[0] = 1, u64(0)) : impl::limbs_add(out, out, size, &one, 1);
        if (size == 0)
            size = 1;
        if (carry != 0)
            out[size++] = carry;
    }
    _set(size, negative);
    free_if(replaced);
}

namespace
{
/// limbs_divrem with its scratch on the stack or, past that, the heap; apart, so a small division keeps a small frame.
CC_DONT_INLINE void divrem_with_scratch(u64* q, u64* r, u64 const* a, i32 an, u64 const* b, i32 bn)
{
    auto const scratch_n = tg::impl::limbs_divrem_scratch(an, bn);
    if (scratch_n <= 2 * stack_limbs)
    {
        u64 scratch[2 * stack_limbs];
        tg::impl::limbs_divrem(q, r, a, an, b, bn, scratch);
    }
    else
    {
        auto scratch = cc::vector<u64>();
        scratch.resize_to_uninitialized(scratch_n);
        tg::impl::limbs_divrem(q, r, a, an, b, bn, scratch.data());
    }
}
} // namespace

void tg::div_mod(bigint& q, bigint& r, bigint const& a, bigint const& b)
{
    CC_ASSERT(!b.is_zero(), "division by zero");
    CC_ASSERT(&q != &r, "the quotient and the remainder are two values");
    if (&q == &a || &q == &b || &r == &a || &r == &b) [[unlikely]]
    {
        // the results overwrite what the division still reads, so they are computed apart
        bigint tq;
        bigint tr;
        div_mod(tq, tr, a, b);
        q = cc::move(tq);
        r = cc::move(tr);
        return;
    }

    auto const an = a.size();
    auto const bn = b.size();
    auto const* const ap = an == 0 ? nullptr : impl::bigint_limbs(a._node);
    auto const* const bp = impl::bigint_limbs(b._node);
    if (an == 0 || impl::limbs_cmp(ap, an, bp, bn) < 0)
    {
        if (q._node != nullptr)
            q._set(0, false);
        r = a;
        return;
    }

    auto const q_negative = a._node->negative != b._node->negative;
    auto const r_negative = a._node->negative;
    auto const qn = an - bn + 1;
    // neither result is an operand, so a replaced node is free to go at once
    impl::bigint_header* replaced = nullptr;
    auto* const qd = q._room(qn, replaced);
    free_if(replaced);
    auto* const rd = r._room(bn, replaced);
    free_if(replaced);

    if (bn == 1)
    {
        rd[0] = impl::limbs_divrem_1(qd, ap, an, bp[0]);
        q._set(impl::limbs_normalized_size(qd, qn), q_negative);
        r._set(rd[0] != 0 ? 1 : 0, r_negative);
        return;
    }

    if (bn == 2)
        impl::limbs_divrem_2(qd, rd, ap, an, bp);
    else
        divrem_with_scratch(qd, rd, ap, an, bp, bn);
    q._set(impl::limbs_normalized_size(qd, qn), q_negative);
    r._set(impl::limbs_normalized_size(rd, bn), r_negative);
}

tg::div_mod_result<tg::bigint> tg::div_mod(bigint const& a, bigint const& b)
{
    div_mod_result<bigint> res;
    div_mod(res.quotient, res.remainder, a, b);
    return res;
}

tg::bigint tg::gcd(bigint const& a, bigint const& b)
{
    // Euclid, its three values reusing their nodes from step to step
    auto x = abs(a);
    auto y = abs(b);
    bigint q;
    bigint r;
    while (!y.is_zero())
    {
        div_mod(q, r, x, y); // x and y are nonnegative, so r is too
        cc::swap(x, y);
        cc::swap(y, r);
    }
    return x;
}
