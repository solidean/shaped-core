#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <clean-core/math/bit.hh>

#include <type_traits>

// Byte-order-explicit scalar loads and stores over a byte buffer.
//
// These are the durable-format primitives.
// A file, a wire protocol or a content hash fixes one byte order, and reading it must never depend on the host's.
//
// Everything here is byte-wise, so neither the buffer's alignment nor the host's endianness enters into it.
// That costs nothing: after inlining a load is one move, plus one byte swap on the opposite-endian side.

namespace cc
{
namespace impl
{
template <isize Size>
struct endian_unsigned;

template <>
struct endian_unsigned<1>
{
    using type = u8;
};
template <>
struct endian_unsigned<2>
{
    using type = u16;
};
template <>
struct endian_unsigned<4>
{
    using type = u32;
};
template <>
struct endian_unsigned<8>
{
    using type = u64;
};

template <isize Size>
using endian_unsigned_t = typename endian_unsigned<Size>::type;
} // namespace impl

/// What load_bytes_* and store_bytes_* accept: an integer or floating-point type of 1, 2, 4 or 8 bytes.
///
/// `bool` is deliberately excluded.
/// It has no unique object representation, so a stored byte other than 0 or 1 would produce a bool that is neither true nor false.
/// Load it as a u8 and validate that byte yourself — which is a format rule you have to state anyway.
template <class T>
concept byte_order_scalar
    = (std::is_integral_v<T> || std::is_floating_point_v<T>) && !std::is_same_v<std::remove_cv_t<T>, bool>
   && (sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8);

/// Reads a T stored little-endian at `offset`, least significant byte first.
/// `offset` must be >= 0 and `offset + sizeof(T)` must be within `bytes`.
template <byte_order_scalar T>
[[nodiscard]] constexpr T load_bytes_le(cc::span<byte const> bytes, isize offset = 0)
{
    CC_ASSERT(offset >= 0 && offset + isize(sizeof(T)) <= bytes.size(), "load_bytes_le reads outside the buffer");

    u64 bits = 0;
    for (isize i = 0; i < isize(sizeof(T)); ++i)
        bits |= u64(u8(bytes[offset + i])) << (i * 8);

    return cc::bit_cast<T>(impl::endian_unsigned_t<sizeof(T)>(bits));
}

/// Reads a T stored big-endian at `offset`, most significant byte first.
/// `offset` must be >= 0 and `offset + sizeof(T)` must be within `bytes`.
template <byte_order_scalar T>
[[nodiscard]] constexpr T load_bytes_be(cc::span<byte const> bytes, isize offset = 0)
{
    CC_ASSERT(offset >= 0 && offset + isize(sizeof(T)) <= bytes.size(), "load_bytes_be reads outside the buffer");

    u64 bits = 0;
    for (isize i = 0; i < isize(sizeof(T)); ++i)
        bits = (bits << 8) | u64(u8(bytes[offset + i]));

    return cc::bit_cast<T>(impl::endian_unsigned_t<sizeof(T)>(bits));
}

/// Writes `value` little-endian at `offset`, least significant byte first.
/// `offset` must be >= 0 and `offset + sizeof(T)` must be within `bytes`.
template <byte_order_scalar T>
constexpr void store_bytes_le(cc::span<byte> bytes, isize offset, T value)
{
    CC_ASSERT(offset >= 0 && offset + isize(sizeof(T)) <= bytes.size(), "store_bytes_le writes outside the buffer");

    auto const bits = u64(cc::bit_cast<impl::endian_unsigned_t<sizeof(T)>>(value));
    for (isize i = 0; i < isize(sizeof(T)); ++i)
        bytes[offset + i] = byte((bits >> (i * 8)) & 0xFF);
}

/// Writes `value` big-endian at `offset`, most significant byte first.
/// `offset` must be >= 0 and `offset + sizeof(T)` must be within `bytes`.
template <byte_order_scalar T>
constexpr void store_bytes_be(cc::span<byte> bytes, isize offset, T value)
{
    CC_ASSERT(offset >= 0 && offset + isize(sizeof(T)) <= bytes.size(), "store_bytes_be writes outside the buffer");

    auto const bits = u64(cc::bit_cast<impl::endian_unsigned_t<sizeof(T)>>(value));
    for (isize i = 0; i < isize(sizeof(T)); ++i)
        bytes[offset + i] = byte((bits >> ((isize(sizeof(T)) - 1 - i) * 8)) & 0xFF);
}
} // namespace cc
