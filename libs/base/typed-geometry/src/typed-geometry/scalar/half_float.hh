#pragma once

#include <clean-core/common/hash.hh>
#include <clean-core/string/format.hh>
#include <typed-geometry/scalar/constants.hh>
#include <typed-geometry/scalar/fwd.hh>
#include <typed-geometry/scalar/impl/half_float_convert.hh>
#include <typed-geometry/scalar/impl/half_float_ops.hh>
#include <typed-geometry/scalar/traits.hh>

#include <compare>

// tg::half_float, spelled tg::f16: an IEEE 754 binary16 value.
// The bit-level bodies live in impl/half_float_convert.hh and impl/half_float_ops.hh.
// libs/base/typed-geometry/docs/modules/scalar.md has the design, and libs/base/typed-geometry/cheat-sheet.md the surface.

/// An IEEE 754 binary16 value: one sign bit, five exponent bits, ten fraction bits.
/// The largest finite value is 65504 and the precision is about 3.3 decimal digits.
///
/// Conversions are explicit both ways.
/// Widening is exact; narrowing rounds to nearest with ties to even, overflows to infinity and keeps a NaN a NaN.
/// Arithmetic computes in f32 and rounds once, which for + - * / and sqrt is exactly binary16's own arithmetic.
/// It matches a GPU computing in half only for unfused `+ - *` under round-to-nearest-even with subnormals kept; each operation in a chain rounds.
/// For heavy math, widen once, compute in f32, and narrow at the end.
///
///     auto const h = tg::f16(0.1f);           // 0.0999755859375
///     auto const x = f32(h) * 3.0f;           // widening is spelled out
///     auto const y = h + tg::f16(1);          // f16 arithmetic
struct tg::half_float
{
    // construction
public:
    half_float() = default;

    explicit constexpr half_float(f32 value) : _bits(tg::impl::f32_to_half_bits(value)) {}

    /// Rounds once, directly from the f64.
    explicit constexpr half_float(f64 value) : _bits(tg::impl::f64_to_half_bits_portable(value)) {}

    /// Rounds once: every integer an f16 does not overflow on is exact in an f64.
    template <class I>
        requires(tg::impl::half_float_integer<I>)
    explicit constexpr half_float(I value) : _bits(tg::impl::f64_to_half_bits_portable(f64(value)))
    {
    }

    [[nodiscard]] static constexpr half_float make_from_bits(u16 bits)
    {
        auto h = half_float();
        h._bits = bits;
        return h;
    }

    // special values
public:
    static half_float const max;        // 65504
    static half_float const lowest;     // -65504
    static half_float const min_normal; // 2^-14
    static half_float const denorm_min; // 2^-24
    static half_float const epsilon;    // 2^-10, the gap between 1 and the next value
    static half_float const infinity;
    static half_float const quiet_nan;

    // access
public:
    [[nodiscard]] constexpr u16 bits() const { return _bits; }

    [[nodiscard]] constexpr f32 to_f32() const { return tg::impl::half_bits_to_f32(_bits); }
    [[nodiscard]] constexpr f64 to_f64() const { return f64(this->to_f32()); }

    explicit constexpr operator f32() const { return this->to_f32(); }
    explicit constexpr operator f64() const { return this->to_f64(); }

    // classification
public:
    [[nodiscard]] constexpr bool is_nan() const { return tg::impl::half_bits_are_nan(_bits); }
    [[nodiscard]] constexpr bool is_inf() const { return (_bits & 0x7fff) == 0x7c00; }
    [[nodiscard]] constexpr bool is_finite() const { return (_bits & 0x7c00) != 0x7c00; }
    [[nodiscard]] constexpr bool is_subnormal() const { return (_bits & 0x7c00) == 0 && (_bits & 0x03ff) != 0; }

    /// Set for -0 and for a NaN with its sign bit set too, which a comparison with zero cannot tell.
    [[nodiscard]] constexpr bool sign_bit() const { return (_bits & 0x8000) != 0; }

    // comparison, on the bits: -0 == +0, and a NaN is unordered with everything, itself included
public:
    [[nodiscard]] friend constexpr bool operator==(half_float a, half_float b)
    {
        return !a.is_nan() && !b.is_nan()
            && tg::impl::half_bits_order_key(a._bits) == tg::impl::half_bits_order_key(b._bits);
    }
    [[nodiscard]] friend constexpr std::partial_ordering operator<=>(half_float a, half_float b)
    {
        if (a.is_nan() || b.is_nan())
            return std::partial_ordering::unordered;
        return tg::impl::half_bits_order_key(a._bits) <=> tg::impl::half_bits_order_key(b._bits);
    }

    // arithmetic, through f32 with one rounding
public:
    [[nodiscard]] friend constexpr half_float operator-(half_float a) { return make_from_bits(u16(a._bits ^ 0x8000)); }

    [[nodiscard]] friend constexpr half_float operator+(half_float a, half_float b)
    {
        return make_from_bits(tg::impl::half_bits_arithmetic(a._bits, b._bits, tg::impl::half_operation::add));
    }
    [[nodiscard]] friend constexpr half_float operator-(half_float a, half_float b)
    {
        return make_from_bits(tg::impl::half_bits_arithmetic(a._bits, b._bits, tg::impl::half_operation::subtract));
    }
    [[nodiscard]] friend constexpr half_float operator*(half_float a, half_float b)
    {
        return make_from_bits(tg::impl::half_bits_arithmetic(a._bits, b._bits, tg::impl::half_operation::multiply));
    }
    [[nodiscard]] friend constexpr half_float operator/(half_float a, half_float b)
    {
        return make_from_bits(tg::impl::half_bits_arithmetic(a._bits, b._bits, tg::impl::half_operation::divide));
    }

    constexpr half_float& operator+=(half_float b) { return *this = *this + b; }
    constexpr half_float& operator-=(half_float b) { return *this = *this - b; }
    constexpr half_float& operator*=(half_float b) { return *this = *this * b; }
    constexpr half_float& operator/=(half_float b) { return *this = *this / b; }

private:
    u16 _bits = 0;
};

namespace tg
{
inline constexpr half_float half_float::max = half_float::make_from_bits(0x7bff);
inline constexpr half_float half_float::lowest = half_float::make_from_bits(0xfbff);
inline constexpr half_float half_float::min_normal = half_float::make_from_bits(0x0400);
inline constexpr half_float half_float::denorm_min = half_float::make_from_bits(0x0001);
inline constexpr half_float half_float::epsilon = half_float::make_from_bits(0x1400);
inline constexpr half_float half_float::infinity = half_float::make_from_bits(0x7c00);
inline constexpr half_float half_float::quiet_nan = half_float::make_from_bits(0x7e00);

/// The primary template narrows a long double, which is ambiguous between the f32 and f64 constructors.
template <>
inline constexpr half_float pi<half_float> = half_float(3.14159265358979323846);

namespace literals
{
/// The compiler has already rounded the decimal literal, and it narrows through f64, so a literal within half an f64 ulp
/// of the midpoint between two halves can round the wrong way.
[[nodiscard]] constexpr half_float operator""_f16(long double v)
{
    return half_float(f64(v));
}
[[nodiscard]] constexpr half_float operator""_f16(unsigned long long v)
{
    return half_float(v);
}
} // namespace literals

using namespace literals;
} // namespace tg

/// Every capability family: sqrt and the arithmetic are exact binary16 operations, abs, rounding and base two are exact
/// on the bits, and the transcendental ones call f32's libm and round once.
template <>
struct tg::scalar_traits<tg::half_float>
{
    using T = tg::half_float;
    using F = tg::scalar_traits<f32>;

    static constexpr bool has_sqrt = true;
    static constexpr bool has_trigonometry = true;
    static constexpr bool has_exponential = true;
    static constexpr bool has_rounding = true;
    static constexpr bool has_abs = true;
    static constexpr bool has_pow2 = true;

    [[nodiscard]] static constexpr T one() { return T::make_from_bits(0x3c00); }
    [[nodiscard]] static constexpr bool is_zero(T x) { return (x.bits() & 0x7fff) == 0; }
    [[nodiscard]] static constexpr bool is_one(T x) { return x.bits() == 0x3c00; }
    [[nodiscard]] static constexpr T abs(T x) { return T::make_from_bits(u16(x.bits() & 0x7fff)); }

    [[nodiscard]] static T sqrt(T x) { return T(F::sqrt(x.to_f32())); }
    [[nodiscard]] static T sin(T x) { return T(F::sin(x.to_f32())); }
    [[nodiscard]] static T cos(T x) { return T(F::cos(x.to_f32())); }
    [[nodiscard]] static T tan(T x) { return T(F::tan(x.to_f32())); }
    [[nodiscard]] static T asin(T x) { return T(F::asin(x.to_f32())); }
    [[nodiscard]] static T acos(T x) { return T(F::acos(x.to_f32())); }
    [[nodiscard]] static T atan(T x) { return T(F::atan(x.to_f32())); }
    [[nodiscard]] static T atan2(T y, T x) { return T(F::atan2(y.to_f32(), x.to_f32())); }
    [[nodiscard]] static T pow(T base, T exponent) { return T(F::pow(base.to_f32(), exponent.to_f32())); }
    [[nodiscard]] static T exp(T x) { return T(F::exp(x.to_f32())); }
    [[nodiscard]] static T log(T x) { return T(F::log(x.to_f32())); }

    [[nodiscard]] static constexpr T round(T x) { return rounded(x, tg::impl::half_rounding::nearest_away); }
    [[nodiscard]] static constexpr T floor(T x) { return rounded(x, tg::impl::half_rounding::floor); }
    [[nodiscard]] static constexpr T ceil(T x) { return rounded(x, tg::impl::half_rounding::ceil); }

    [[nodiscard]] static T scale_by_pow2(T x, int n)
    {
        return T::make_from_bits(tg::impl::half_bits_scale_by_pow2(x.bits(), n));
    }

    /// x must be finite and non-zero: a subnormal is normalized, but zero and the non-finites have no exponent.
    [[nodiscard]] static pow2_split<T> split_pow2(T x)
    {
        auto const split = tg::impl::half_bits_split_pow2(x.bits());
        return {.significand = T::make_from_bits(split.significand), .exponent = split.exponent};
    }

private:
    [[nodiscard]] static constexpr T rounded(T x, tg::impl::half_rounding mode)
    {
        return T::make_from_bits(tg::impl::half_bits_round(x.bits(), mode));
    }
};

/// The bits, with -0 folded onto +0 so the two zeros hash equally, as the float hashes do.
template <>
struct cc::custom::hash_trait<tg::half_float>
{
    [[nodiscard]] static constexpr u64 hash(tg::half_float v) { return v.bits() == 0x8000 ? 0 : u64(v.bits()); }
};

/// Takes a float spec.
/// Without a presentation type it prints the shortest digits that read back as the same f16, so `tg::f16(0.1f)` prints `0.1`; that same rule prints the largest f16 as `65500`.
/// With `f`, `e` or `g` it prints the exact value to that precision, `{:.0f}` giving `65504`.
template <>
struct cc::custom::formatter<tg::half_float>
{
    static consteval void validate(cc::string_view spec) { cc::validate_float_format_spec(spec); }
    static void format(cc::format_sink out, cc::string_view spec, tg::half_float const& v)
    {
        if (v.is_finite() && cc::is_shortest_float_format_spec(spec))
            cc::format_value(out, spec, tg::impl::half_float_shortest_value(v));
        else
            cc::format_value(out, spec, v.to_f32());
    }
};
