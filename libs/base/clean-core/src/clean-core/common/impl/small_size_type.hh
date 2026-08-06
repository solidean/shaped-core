#pragma once

#include <clean-core/fwd.hh>

/// cc::impl::small_size_t<MaxCount, MinAlign> — the smallest **unsigned** type that stores a count in [0, MaxCount] next to `MinAlign`-aligned data.
/// It picks the smallest of {u8, u16, u32, u64} that both represents MaxCount and is at least MinAlign bytes wide.
/// The size field then occupies what would otherwise be tail padding, instead of adding bytes.
///
/// A storage type only: inline containers convert to isize at their public boundary, so the unsigned storage never mixes with the signed API.
///
/// Examples: fixed_vector<u8, 10> -> u8; fixed_vector<u8, 300> -> u16 (u8 can't hold 300);
/// fixed_vector<u64, 2> -> u64 (alignof(u64) == 8, so a smaller field would just be padding).

namespace cc::impl
{
[[nodiscard]] consteval bool small_size_fits(u64 max_count, int bytes)
{
    if (bytes >= 8)
        return true; // isize covers any realistic count
    return max_count <= ((u64(1) << (8 * bytes)) - 1);
}

/// Byte width (1/2/4/8) of the chosen type: the smallest power of two that is >= MinAlign (capped at 8)
/// and wide enough to represent MaxCount.
[[nodiscard]] consteval int small_size_bytes(u64 max_count, u64 min_align)
{
    int bytes = 1;
    while (bytes < 8 && (u64(bytes) < min_align || !small_size_fits(max_count, bytes)))
        bytes *= 2;
    return bytes;
}

template <int Bytes>
struct small_size_type_of;
template <>
struct small_size_type_of<1>
{
    using type = u8;
};
template <>
struct small_size_type_of<2>
{
    using type = u16;
};
template <>
struct small_size_type_of<4>
{
    using type = u32;
};
template <>
struct small_size_type_of<8>
{
    using type = u64;
};

/// Smallest unsigned integer type holding a count in [0, MaxCount], at least MinAlign bytes wide.
/// MinAlign defaults to 1, which is the pure smallest-uint-for-value case; the header comment has the rationale.
template <u64 MaxCount, u64 MinAlign = 1>
using small_size_t = typename small_size_type_of<small_size_bytes(MaxCount, MinAlign)>::type;
} // namespace cc::impl
