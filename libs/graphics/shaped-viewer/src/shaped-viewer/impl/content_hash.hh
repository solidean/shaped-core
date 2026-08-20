#pragma once

#include <clean-core/bytes/hash128.hh> // cc::hash128
#include <clean-core/container/span.hh>
#include <shaped-viewer/fwd.hh>

// The content-hash seeds sv keys its cached resources by.
//
// A seed lives here, once, because two types may hash the SAME payload and must agree: an sv::geometry and the
// sv::triangle_data it is handed to a manager as are one buffer, so they must produce one key or the manager
// caches — and uploads — the same bytes twice.
// A seed also separates the kinds: equal bytes read as positions and as an attribute are different resources.

namespace sv::impl
{
inline constexpr u64 position_hash_seed = 0x4358345;
inline constexpr u64 index_hash_seed = 0x623435;
inline constexpr u64 material_hash_seed = 0x523453;
inline constexpr u64 attribute_hash_seed = 0x7a11b2;

/// Joins two payload digests into one key, in order.
/// Separate allocations cannot be hashed as one range, so a multi-buffer resource hashes each buffer and folds the digests here.
[[nodiscard]] inline cc::hash128 combine_digests(cc::hash128 a, cc::hash128 b)
{
    cc::hash128 const digests[] = {a, b};
    return cc::hash128::create(cc::span<cc::hash128 const>(digests).as_bytes(), 0);
}
} // namespace sv::impl
