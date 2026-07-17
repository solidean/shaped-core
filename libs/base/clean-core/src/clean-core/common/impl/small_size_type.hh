#pragma once

#include <clean-core/fwd.hh>

/// cc::impl::small_size_t<MaxCount, MinAlign> — the smallest **unsigned** integer type suitable for
/// storing a size/count in [0, MaxCount] inline next to `MinAlign`-aligned data. An internal helper for
/// inline containers (fixed_vector, small string buffers, …) that keep a size field beside aligned
/// storage: it picks the smallest of {u8, u16, u32, u64} that both represents MaxCount and is at least
/// MinAlign bytes wide, so the size field occupies what would otherwise be tail padding rather than adding
/// bytes. It is only ever a storage type — containers convert to isize at their public boundary (so the
/// unsigned storage never mixes with the signed API).
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

/// Smallest unsigned integer type holding a count in [0, MaxCount], at least MinAlign bytes wide (default
/// 1, i.e. the pure smallest-uint-for-value). See the header comment for the rationale.
template <u64 MaxCount, u64 MinAlign = 1>
using small_size_t = typename small_size_type_of<small_size_bytes(MaxCount, MinAlign)>::type;
} // namespace cc::impl
