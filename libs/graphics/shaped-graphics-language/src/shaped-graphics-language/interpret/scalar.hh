#pragma once

#include <shaped-graphics-language/fwd.hh>

/// The leaves every value of the abstract machine is made of.
/// A header of its own, since the builtin registry names them and stands below the check pass.

enum class sgl::check::value_kind : sgl::u8
{
    none,
    scalar_float,
    scalar_int,
    scalar_uint,
    boolean,
    /// The 16-bit families (CHK-381), whose bits are the low 16 of `scalar::bits` and whose upper 16 are zero.
    scalar_half,
    scalar_short,
    scalar_ushort,
    /// The 64-bit families (CHK-450), whose low 32 bits are `scalar::bits` and whose high 32 are `scalar::high`.
    scalar_long,
    scalar_ulong,
    scalar_double,
};

/// One leaf of a value, held as its bits so that equality is exact: a NaN equals itself and 0.0 differs from -0.0.
struct sgl::check::scalar
{
    value_kind kind = value_kind::none;
    u32 bits = 0;
    /// The high 32 bits of a 64-bit leaf, and 0 for every other, so equality stays exact either way.
    u32 high = 0;

    [[nodiscard]] static scalar of(f32 v);
    [[nodiscard]] static scalar of(i32 v);
    [[nodiscard]] static scalar of(bool v);
    /// Named rather than overloaded, so an integer literal never has to pick between `i32` and `u32`.
    [[nodiscard]] static scalar of_uint(u32 v) { return {.kind = value_kind::scalar_uint, .bits = v}; }
    /// The half nearest `v`, ties to even, rounded once from the exact value.
    [[nodiscard]] static scalar of_half(f64 v);
    [[nodiscard]] static scalar of_long(i64 v) { return of_wide(value_kind::scalar_long, u64(v)); }
    [[nodiscard]] static scalar of_ulong(u64 v) { return of_wide(value_kind::scalar_ulong, v); }
    [[nodiscard]] static scalar of_double(f64 v);
    /// A 64-bit leaf of `kind` holding `bits`.
    [[nodiscard]] static scalar of_wide(value_kind kind, u64 bits)
    {
        return {.kind = kind, .bits = u32(bits), .high = u32(bits >> 32)};
    }

    [[nodiscard]] f32 as_float() const;
    [[nodiscard]] i32 as_int() const { return i32(bits); }
    [[nodiscard]] u32 as_uint() const { return bits; }
    [[nodiscard]] bool as_bool() const { return bits != 0; }
    /// The 64 bits of a 64-bit leaf; of any other, its 32 bits.
    [[nodiscard]] u64 wide_bits() const { return u64(high) << 32 | bits; }
    [[nodiscard]] i64 as_long() const { return i64(wide_bits()); }
    [[nodiscard]] u64 as_ulong() const { return wide_bits(); }
    [[nodiscard]] f64 as_double() const;

    /// A 16-bit leaf as the 32-bit leaf of its family holding the same value, which every evaluator reads.
    /// Any other leaf is itself.
    [[nodiscard]] scalar widened() const;
    /// A 32-bit leaf of the family of `to` as a leaf of `to`: a float rounded to the nearest half, ties to even, and an
    /// integer cut to its low 16 bits.
    /// Any other pair is the leaf itself.
    [[nodiscard]] scalar narrowed_to(value_kind to) const;

    constexpr bool operator==(scalar const&) const = default;
};

namespace sgl::check
{
/// `scalar_half`, `scalar_short` and `scalar_ushort`.
[[nodiscard]] constexpr bool is_16_bit(value_kind k)
{
    return k == value_kind::scalar_half || k == value_kind::scalar_short || k == value_kind::scalar_ushort;
}

/// `scalar_long`, `scalar_ulong` and `scalar_double`.
[[nodiscard]] constexpr bool is_64_bit(value_kind k)
{
    return k == value_kind::scalar_long || k == value_kind::scalar_ulong || k == value_kind::scalar_double;
}

/// The bytes a scalar of `k` takes in GPU memory: 2, 4 or 8.
[[nodiscard]] constexpr i32 scalar_bytes_of(value_kind k)
{
    return is_16_bit(k) ? 2 : is_64_bit(k) ? 8 : 4;
}

/// The 32-bit kind of the family of `k`: `scalar_float` for `scalar_half`; any other kind is itself.
[[nodiscard]] constexpr value_kind wide_kind_of(value_kind k)
{
    switch (k)
    {
    case value_kind::scalar_half:
        return value_kind::scalar_float;
    case value_kind::scalar_short:
        return value_kind::scalar_int;
    case value_kind::scalar_ushort:
        return value_kind::scalar_uint;
    default:
        return k;
    }
}
} // namespace sgl::check
