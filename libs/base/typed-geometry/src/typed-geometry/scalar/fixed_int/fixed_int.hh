#pragma once

#include <clean-core/function/function_ref.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/string.hh>
#include <typed-geometry/scalar/fixed_int/impl/core.hh>
#include <typed-geometry/scalar/traits.hh>

// tg::fixed_int<Bits> / tg::fixed_uint<Bits>: fixed-width two's-complement integers whose operators all wrap.
// The heterogeneous arithmetic — tg::add / sub / mul<R>, the division family, the bounded quotient — is fixed_arith.hh.
// libs/base/typed-geometry/docs/modules/scalar.md has the design, and libs/base/typed-geometry/cheat-sheet.md the surface.

namespace tg::impl
{
/// m's digits in base 2, 8, 10 or 16, most significant first, written into the tail of buf.
/// buf must hold Bits + 1 characters, which is enough for every base.
template <int Bits>
[[nodiscard]] cc::string_view write_digits(fixed_integer<Bits, false> const& m, int base, bool upper, cc::span<char> buf)
{
    auto i = buf.size();
    auto const digit = [upper](int d) { return char(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); };

    if (base == 10)
    {
        // Peel off 19 decimal digits at a time with one 128 ÷ 64 step per word.
        constexpr int count = word_count<Bits>;
        constexpr u64 chunk = 10'000'000'000'000'000'000ull;
        u64 w[count] = {};
        for (auto k = 0; k < count; ++k)
            w[k] = limb(m, k);
        while (true)
        {
            u64 rem = 0;
            auto rest = false;
            for (auto k = count - 1; k >= 0; --k)
            {
                auto const d = cc::udiv128({w[k], rem}, chunk);
                w[k] = d.quotient;
                rem = d.remainder;
                rest = rest || d.quotient != 0;
            }
            for (auto n = 0; n < 19 && (rest || rem != 0 || n == 0); ++n)
            {
                buf[--i] = digit(int(rem % 10));
                rem /= 10;
            }
            if (!rest)
                break;
        }
    }
    else
    {
        auto const k = base == 16 ? 4 : base == 8 ? 3 : 1;
        for (auto pos = 0; pos < Bits; pos += k)
        {
            auto const word = pos >> 6;
            auto const bit = pos & 63;
            auto const v = (limb(m, word) >> bit) | ((limb(m, word + 1) << 1) << (63 - bit));
            buf[--i] = digit(int(v & ((u64(1) << k) - 1)));
        }
        while (i < buf.size() - 1 && buf[i] == '0')
            ++i;
    }
    return cc::string_view(buf.data() + i, buf.size() - i);
}
} // namespace tg::impl

/// Takes clean-core's integer spec: d/x/X/o/b/B, `#`, sign, grouping, fill and width.
/// A negative value prints as a sign and a magnitude, like the builtins do.
template <int Bits, bool Signed>
struct cc::custom::formatter<tg::impl::fixed_integer<Bits, Signed>>
{
    static consteval void validate(cc::string_view spec) { cc::validate_integer_format_spec(spec); }
    static void format(cc::format_sink out, cc::string_view spec, tg::impl::fixed_integer<Bits, Signed> const& v)
    {
        char buf[Bits + 1];
        auto const m = tg::impl::magnitude(v);
        cc::format_wide_integer(out, spec, v.is_negative(), [&](int base, bool upper)
                                { return tg::impl::write_digits(m, base, upper, cc::span<char>(buf, Bits + 1)); });
    }
};

template <int Bits, bool Signed>
cc::string tg::impl::fixed_integer<Bits, Signed>::to_string() const
{
    return cc::format("{}", *this);
}

/// A fixed_int is a scalar with the integer capabilities, so tg::vec<3, tg::fi64> and tg::pos<2, tg::fi128> exist.
/// Their operators wrap at the element width: a dot product over fi64 is computed in fi64.
template <int Bits, bool Signed>
struct tg::scalar_traits<tg::impl::fixed_integer<Bits, Signed>>
{
    using T = tg::impl::fixed_integer<Bits, Signed>;

    static constexpr bool has_sqrt = false;
    static constexpr bool has_trigonometry = false;
    static constexpr bool has_exponential = false;
    static constexpr bool has_rounding = false;
    static constexpr bool has_abs = true;
    static constexpr bool has_pow2 = false;

    [[nodiscard]] static constexpr T one() { return T(1); }
    [[nodiscard]] static constexpr bool is_zero(T const& x) { return x == T(); }
    [[nodiscard]] static constexpr bool is_one(T const& x) { return x == T(1); }

    /// x must not be the most negative value, which has no representable magnitude.
    [[nodiscard]] static constexpr T abs(T const& x) { return x.is_negative() ? -x : x; }
};
