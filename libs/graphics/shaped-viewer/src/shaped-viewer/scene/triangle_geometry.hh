#pragma once

#include <clean-core/bytes/hash128.hh>  // cc::hash128
#include <clean-core/common/utility.hh> // cc::forward, cc::move
#include <clean-core/container/pinned_data.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/impl/content_hash.hh>
#include <typed-geometry/geometry/primitives/triangle.hh>
#include <typed-geometry/linalg/pos.hh>

/// A triangle surface in either of its two layouts — a raw triangle list or an indexed one — behind one type.
///
/// `indices` empty means raw: 3 consecutive positions form a triangle, so the position count must be a multiple of 3.
/// Otherwise the positions are a pool the indices name: 3 consecutive indices form a triangle, each index < positions.size().
/// Triangle order — and with it `PrimitiveIndex()`, which per-triangle data is indexed by — follows the index buffer whenever there is one.
///
/// Triangles are the only surface sv traces, so this is the only geometry kind today; a point cloud or a volume would be its own type next to it, not a variant inside it.
///
/// Both buffers are pinned, so the geometry keeps its memory alive whatever the authoring range does next.
/// `hash` is the content key the resource managers cache by: equal contents must carry equal hashes, or an acquire hands back the wrong geometry.
/// The two named constructors are the only way to fill one — they are what keeps payload and hash in sync.
/// They hash through sv::impl's shared seeds, so the key survives the trip through `triangle_data::from` / `indexed_triangle_data::from`.
struct sv::triangle_geometry
{
    cc::pinned_data<tg::pos3f const> positions;

    /// empty for a raw triangle list; otherwise 3 indices per triangle
    cc::pinned_data<u32 const> indices;

    cc::hash128 hash;

    /// Pins `triangles` and hashes their bytes.
    ///
    /// A tg::triangle3f is three contiguous positions, so the pin is reinterpreted onto the position buffer rather than copied:
    /// the stored positions ARE the caller's triangles, and the "multiple of 3" precondition is the type's rather than a runtime check.
    ///
    /// The parameter is a template because `cc::make_pinned_data` picks its strategy from what it is handed —
    /// a pin or shared_ptr is shared as is, an owning rvalue is moved in, and only a borrow or an lvalue is deep-copied.
    /// A `cc::span` parameter would collapse all four onto the copy.
    template <class Triangles>
    [[nodiscard]] static triangle_geometry create_from_triangles(Triangles&& triangles)
    {
        // A triangle IS its three positions, with nothing around them — which is what lets the pin be reinterpreted rather than unpacked.
        static_assert(sizeof(tg::triangle3f) == 3 * sizeof(tg::pos3f));

        cc::pinned_data<tg::triangle3f const> pinned = cc::make_pinned_data(cc::forward<Triangles>(triangles));
        auto positions = pinned.reinterpret_as<tg::pos3f const>();
        auto const hash = cc::hash128::create(positions.span().as_bytes(), impl::position_hash_seed);
        return {.positions = cc::move(positions), .hash = hash};
    }

    /// Pins `positions` as a raw triangle list — 3 consecutive positions per triangle, so the count must be a multiple
    /// of 3 — and hashes their bytes.
    ///
    /// The same payload and key `triangle_data::create` produces, for a caller who holds positions rather than
    /// `tg::triangle3f`s.
    template <class Positions>
    [[nodiscard]] static triangle_geometry create_from_positions(Positions&& positions)
    {
        cc::pinned_data<tg::pos3f const> pinned = cc::make_pinned_data(cc::forward<Positions>(positions));
        auto const hash = cc::hash128::create(pinned.span().as_bytes(), impl::position_hash_seed);
        return {.positions = cc::move(pinned), .hash = hash};
    }

    /// Pins both buffers and combines their digests into the content hash.
    /// `indices.size()` must be a multiple of 3, and every index must be < `positions.size()`.
    template <class Positions, class Indices>
    [[nodiscard]] static triangle_geometry create_from_indexed_triangles(Positions&& positions, Indices&& indices)
    {
        cc::pinned_data<tg::pos3f const> pinned_positions = cc::make_pinned_data(cc::forward<Positions>(positions));
        cc::pinned_data<u32 const> pinned_indices = cc::make_pinned_data(cc::forward<Indices>(indices));
        auto const hash
            = impl::combine_digests(cc::hash128::create(pinned_positions.span().as_bytes(), impl::position_hash_seed),
                                    cc::hash128::create(pinned_indices.span().as_bytes(), impl::index_hash_seed));
        return {.positions = cc::move(pinned_positions), .indices = cc::move(pinned_indices), .hash = hash};
    }

    /// Geometry that is only a KEY so far: its identity, with no bytes behind it yet.
    ///
    /// This is the smallest form of the plan's `from_uri` recipe, and what lets a structure pass describe a mesh it
    /// has not read — a name, a placement and a box, named by the key its payload will arrive under.
    /// The key must be reproducible from the source alone, and must fold in the importer's version: change how a
    /// primitive is decoded and the key has to change with it, or a stale payload answers for a new recipe.
    ///
    /// A deferred geometry is NOT empty: `create_mesh` mints a pending record for it, drawn as a placeholder, and
    /// supplying the same key later fills that record in place rather than making a second one.
    [[nodiscard]] static triangle_geometry create_deferred(cc::hash128 key) { return {.hash = key}; }

    /// The same payload under a caller-chosen key rather than its own content hash.
    ///
    /// This is the other half of `create_deferred`: a structure pass promises geometry under a recipe key, and the
    /// payload that arrives later has to answer under that same name to fill the record the promise minted.
    ///
    /// The price is real and worth stating: two primitives with identical bytes no longer dedupe, because a recipe
    /// names a SOURCE and not a content.
    /// Use it only where the key is a recipe; ordinary geometry keeps its content hash and keeps the dedup.
    [[nodiscard]] static triangle_geometry rekeyed(triangle_geometry g, cc::hash128 key)
    {
        g.hash = key;
        return g;
    }

    [[nodiscard]] bool is_indexed() const { return !indices.empty(); }

    /// Whether the payload is still to come — a key with no bytes.
    [[nodiscard]] bool is_deferred() const { return positions.empty() && hash != cc::hash128(); }

    /// Whether this describes nothing at all, which is neither bytes nor a promise of them.
    [[nodiscard]] bool is_empty() const { return positions.empty() && !is_deferred(); }

    [[nodiscard]] isize vertex_count() const { return positions.size(); }
    [[nodiscard]] isize triangle_count() const { return (is_indexed() ? indices.size() : positions.size()) / 3; }
};
