#pragma once

#include <clean-core/common/hash128.hh> // cc::hash128
#include <clean-core/common/utility.hh> // cc::forward, cc::move
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/pbr_material.hh>
#include <typed-geometry/linalg/pos.hh>

// What a caller hands a resource manager: the payload plus the content hash that identifies it.
//
// The hash is the manager's whole cache key — equal contents must carry equal hashes, or an acquire hands back
// the wrong resource. Hashing happens out here, once at authoring time; a manager never hashes, so a per-frame
// acquire stays O(1) and the hash load is the caller's to schedule.
//
// The payload is a cc::pinned_data, so it owns (shares) its memory and stays alive across the acquire that uploads it, whatever the caller does with the source range.
// `create` takes an existing pin or any contiguous range, and leaves the strategy to cc::make_pinned_data.
// A pin is shared as is, an owning rvalue is moved in; only a borrow or an lvalue is deep-copied.

namespace sv
{
/// Geometry as a non-indexed triangle list: 3 consecutive positions per triangle, count a multiple of 3.
struct triangle_data
{
    cc::pinned_data<tg::pos3f const> positions;
    cc::hash128 hash;

    /// Pins `positions` and hashes their bytes.
    template <class Positions>
    [[nodiscard]] static triangle_data create(Positions&& positions)
    {
        cc::pinned_data<tg::pos3f const> pinned = cc::make_pinned_data(cc::forward<Positions>(positions));
        auto const hash = cc::hash128::create(pinned.span().as_bytes(), 0x4358345);
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
        cc::pinned_data<tg::pos3f const> pinned_positions = cc::make_pinned_data(cc::forward<Positions>(positions));
        cc::pinned_data<u32 const> pinned_indices = cc::make_pinned_data(cc::forward<Indices>(indices));
        auto const hash_positions = cc::hash128::create(pinned_positions.span().as_bytes(), 0x4358345);
        auto const hash_indices = cc::hash128::create(pinned_indices.span().as_bytes(), 0x623435);
        // The two buffers are separate allocations, so the combined hash is over the digests, not over concatenated contents.
        cc::hash128 const digests[] = {hash_positions, hash_indices};
        auto const hash = cc::hash128::create(cc::span<cc::hash128 const>(digests).as_bytes(), 0);
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
        cc::pinned_data<pbr_material const> pinned = cc::make_pinned_data(cc::forward<Materials>(materials));
        auto const hash = cc::hash128::create(pinned.span().as_bytes(), 0x523453);
        return {.materials = cc::move(pinned), .hash = hash};
    }
};
} // namespace sv
