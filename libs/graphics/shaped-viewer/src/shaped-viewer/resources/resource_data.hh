#pragma once

#include <clean-core/bytes/hash128.hh> // cc::hash128
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::forward, cc::move
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/impl/content_hash.hh>
#include <shaped-viewer/scene/pbr_material.hh>
#include <shaped-viewer/scene/triangle_geometry.hh>
#include <typed-geometry/linalg/pos.hh>

// What a caller hands a resource manager: the payload plus the content hash that identifies it.
//
// The hash is the manager's whole cache key — equal contents must carry equal hashes, or an acquire hands back the wrong resource.
// Hashing happens out here, once at authoring time.
// A manager never hashes, so a per-frame acquire stays O(1) and the hash load is the caller's to schedule.
//
// The payload is a cc::pinned_data, so it owns (shares) its memory and stays alive across the acquire that uploads it, whatever the caller does with the source range.
// `create` takes an existing pin or any contiguous range, and leaves the strategy to cc::make_pinned_data.
// A pin is shared as is, an owning rvalue is moved in; only a borrow or an lvalue is deep-copied.

/// Geometry as a non-indexed triangle list: 3 consecutive positions per triangle, count a multiple of 3.
struct sv::triangle_data
{
    cc::pinned_data<tg::pos3f const> positions;
    cc::hash128 hash;

    /// Pins `positions` and hashes their bytes.
    template <class Positions>
    [[nodiscard]] static triangle_data create(Positions&& positions)
    {
        cc::pinned_data<tg::pos3f const> pinned = cc::make_pinned_data(cc::forward<Positions>(positions));
        auto const hash = cc::hash128::create(pinned.span().as_bytes(), impl::position_hash_seed);
        return {.positions = cc::move(pinned), .hash = hash};
    }

    /// The payload for an authored geometry, sharing its pin and its key rather than re-hashing.
    /// `g` must not be indexed — use indexed_triangle_data::from for that layout.
    [[nodiscard]] static triangle_data from(triangle_geometry const& g)
    {
        CC_ASSERT(!g.is_indexed(), "geometry is indexed - use indexed_triangle_data::from");
        return {.positions = g.positions, .hash = g.hash};
    }
};

/// Geometry as an indexed triangle list: 3 consecutive indices per triangle, each naming a position.
/// `indices.size()` must be a multiple of 3, and every index must be < `positions.size()`.
///
/// Triangle order — and with it `PrimitiveIndex()`, which is what a material set is indexed by — follows the
/// index buffer, not the position buffer.
struct sv::indexed_triangle_data
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
        // The two buffers are separate allocations, so the combined hash is over the digests, not over concatenated contents.
        auto const hash
            = impl::combine_digests(cc::hash128::create(pinned_positions.span().as_bytes(), impl::position_hash_seed),
                                    cc::hash128::create(pinned_indices.span().as_bytes(), impl::index_hash_seed));
        return {.positions = cc::move(pinned_positions), .indices = cc::move(pinned_indices), .hash = hash};
    }

    /// The payload for an authored geometry, sharing its pins and its key rather than re-hashing.
    /// `g` must be indexed — use triangle_data::from for a raw triangle list.
    [[nodiscard]] static indexed_triangle_data from(triangle_geometry const& g)
    {
        CC_ASSERT(g.is_indexed(), "geometry is a raw triangle list - use triangle_data::from");
        return {.positions = g.positions, .indices = g.indices, .hash = g.hash};
    }
};

/// One PBR material per triangle, indexed by `PrimitiveIndex()` in the closest-hit — so the count must match
/// the triangle count of the mesh it is drawn with.
struct sv::material_data
{
    cc::pinned_data<pbr_material const> materials;
    cc::hash128 hash;

    /// Pins `materials` and hashes their bytes.
    template <class Materials>
    [[nodiscard]] static material_data create(Materials&& materials)
    {
        cc::pinned_data<pbr_material const> pinned = cc::make_pinned_data(cc::forward<Materials>(materials));
        auto const hash = cc::hash128::create(pinned.span().as_bytes(), impl::material_hash_seed);
        return {.materials = cc::move(pinned), .hash = hash};
    }
};
