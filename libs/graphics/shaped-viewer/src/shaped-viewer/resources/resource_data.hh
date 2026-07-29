#pragma once

#include <clean-core/common/hash128.hh> // cc::hash128
#include <clean-core/common/utility.hh> // cc::forward, cc::move
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/pbr_material.hh>
#include <typed-geometry/linalg/pos.hh>

#include <type_traits>

// What a caller hands a resource manager: the payload plus the content hash that identifies it.
//
// The hash is the manager's whole cache key — equal contents must carry equal hashes, or an acquire hands back
// the wrong resource. Hashing happens out here, once at authoring time; a manager never hashes, so a per-frame
// acquire stays O(1) and the hash load is the caller's to schedule.
//
// The payload is a cc::pinned_data, so it owns (shares) its memory and stays alive across the acquire that
// uploads it, whatever the caller does with the source range. `create` takes either an existing pin (shared as
// is) or any contiguous range, which cc::make_pinned_data moves in when it can and deep-copies when it cannot.

namespace sv
{
namespace impl
{
/// Pins `r` as a `cc::pinned_data<T const>`, sharing the pin unchanged when `r` already is one.
template <class T, class Range>
[[nodiscard]] cc::pinned_data<T const> pin_as(Range&& r)
{
    if constexpr (std::is_convertible_v<Range&&, cc::pinned_data<T const>>)
        return cc::pinned_data<T const>(cc::forward<Range>(r));
    else
        return cc::pinned_data<T const>(cc::make_pinned_data(cc::forward<Range>(r)));
}
} // namespace impl

/// The XXH3-128 digest of a range's bytes — the primitive behind every `create` below.
template <class T>
[[nodiscard]] cc::hash128 hash_bytes_of(cc::span<T const> values)
{
    return cc::hash128::create(values.as_bytes(), 0);
}

/// Combines per-buffer digests into one content hash by hashing the digests themselves, so each buffer
/// contributes its full 128 bits. Concatenating the raw buffers is not an option — they are separate allocations.
[[nodiscard]] inline cc::hash128 combine_hashes(cc::hash128 a, cc::hash128 b)
{
    cc::hash128 const parts[] = {a, b};
    return cc::hash128::create(cc::span<cc::hash128 const>(parts).as_bytes(), 0);
}

/// Geometry as a non-indexed triangle list: 3 consecutive positions per triangle, count a multiple of 3.
struct triangle_data
{
    cc::pinned_data<tg::pos3f const> positions;
    cc::hash128 hash;

    /// Pins `positions` and hashes their bytes.
    template <class Positions>
    [[nodiscard]] static triangle_data create(Positions&& positions)
    {
        auto pinned = impl::pin_as<tg::pos3f>(cc::forward<Positions>(positions));
        auto const hash = hash_bytes_of(pinned.span());
        return {.positions = cc::move(pinned), .hash = hash};
    }
};

/// Geometry as an indexed triangle list: 3 consecutive indices per triangle, each naming a position.
/// `indices.size()` must be a multiple of 3, and every index must be < `positions.size()`.
///
/// Triangle order — and with it `PrimitiveIndex()`, which is what a material set is indexed by — follows the
/// index buffer, not the position buffer.
struct indexed_triangle_data
{
    cc::pinned_data<tg::pos3f const> positions;
    cc::pinned_data<u32 const> indices;
    cc::hash128 hash;

    /// Pins both buffers and combines their two digests into the content hash.
    template <class Positions, class Indices>
    [[nodiscard]] static indexed_triangle_data create(Positions&& positions, Indices&& indices)
    {
        auto pinned_positions = impl::pin_as<tg::pos3f>(cc::forward<Positions>(positions));
        auto pinned_indices = impl::pin_as<u32>(cc::forward<Indices>(indices));
        auto const hash = combine_hashes(hash_bytes_of(pinned_positions.span()), hash_bytes_of(pinned_indices.span()));
        return {.positions = cc::move(pinned_positions), .indices = cc::move(pinned_indices), .hash = hash};
    }
};

/// One PBR material per triangle, indexed by `PrimitiveIndex()` in the closest-hit — so the count must match
/// the triangle count of the mesh it is drawn with.
struct material_data
{
    cc::pinned_data<pbr_material const> materials;
    cc::hash128 hash;

    /// Pins `materials` and hashes their bytes.
    template <class Materials>
    [[nodiscard]] static material_data create(Materials&& materials)
    {
        auto pinned = impl::pin_as<pbr_material>(cc::forward<Materials>(materials));
        auto const hash = hash_bytes_of(pinned.span());
        return {.materials = cc::move(pinned), .hash = hash};
    }
};
} // namespace sv
