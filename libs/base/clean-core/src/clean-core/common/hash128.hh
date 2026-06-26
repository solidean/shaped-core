#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>

namespace cc
{
/// A 128-bit hash value, stored as two u64 limbs.
/// Structurally comparable; the default operator<=> orders lexicographically by (low, high).
struct hash128
{
    cc::u64 low = 0;
    cc::u64 high = 0;

    [[nodiscard]] friend constexpr bool operator==(hash128 const&, hash128 const&) = default;
    [[nodiscard]] friend constexpr auto operator<=>(hash128 const&, hash128 const&) = default;

    /// Computes the XXH3 128-bit hash of `data` with the given `seed`.
    /// Empty data is valid (hashes the empty input); a seed of 0 selects XXH3's unseeded variant.
    [[nodiscard]] static hash128 create(cc::span<cc::byte const> data, cc::u64 seed);

    /// ADL customization point (see common/hash.hh): a hash128 is already a hash,
    /// so we surface its low limb as the 64-bit hash.
    [[nodiscard]] friend constexpr cc::u64 hash(hash128 const& v) { return v.low; }
};
} // namespace cc
