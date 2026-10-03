#pragma once

#include <babel-serializer/font/font.hh>
#include <clean-core/container/vector.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/impl/lru_pool.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>

/// One acquired drawing set: where each of its drawings' records sits in the manager's atlas.
/// A drawing whose records did not fit has a count of 0, and draws nothing.
struct sv::drawing_set_record
{
    cc::vector<u32> first_record;
    cc::vector<u32> record_count;

    /// Each drawing's extent in its own units, from its outlines' points; empty for a drawing with nothing in it.
    cc::vector<tg::aabb2f> bounds;
};

/// Hands out `drawing_set_id`s and owns the Slug atlas every acquired set's shapes and records live in.
///
/// The drawing counterpart of `mesh_manager`: an acquire is a content-hash lookup, and a miss compiles the set's outlines
/// on the CPU and places them, to be uploaded by `prepare` before the frame's first pass.
///
/// **Nothing is evicted yet.**
/// The atlas is append-only, so an evicted set's space could not be reused; the pool runs without limits until the
/// atlas frees blocks (libs/graphics/shaped-viewer/docs/canvas.md).
class sv::drawing_manager : public impl::lru_pool<drawing_set_id, drawing_set_record>
{
public:
    /// The id for `set`, resident from a prior acquire (O(1) through the set's cache slot), or freshly placed.
    [[nodiscard]] drawing_set_id acquire(drawing_set const& set);

    /// The id for a lone drawing, as a one-element set keyed by the drawing's own hash.
    [[nodiscard]] drawing_set_id acquire(drawing const& d);

    /// The record range of drawing `index` of the set `id` names, which must be resident.
    [[nodiscard]] u32 first_record(drawing_set_id id, u32 index);
    [[nodiscard]] u32 record_count(drawing_set_id id, u32 index);

    /// Glyphs per glyph set: a font's glyphs reach the GPU in fixed sets of this many consecutive glyph ids.
    static constexpr u32 glyphs_per_set = 64;

    /// The set holding glyph `g` of `f` and its neighbours, each glyph a drawing of its outline in font units, y up.
    /// Compiled the first time any of its glyphs is asked for; keyed by the font's hash and the set's place, so the
    /// same glyphs are never compiled twice.
    /// The glyph is drawing `u32(g) % glyphs_per_set` of the set.
    [[nodiscard]] drawing_set_id glyph_set(font const& f, babel::font::glyph_id g);

    /// The pieces annotations are built from, in logical pixels: a disk of `marker_radius` (0), a ring of that outer
    /// radius `ring_width` wide (1), a unit segment from (0, -0.5) to (1, 0.5) (2), and a disk of radius 1 (3).
    /// Keyed by the two sizes, so every annotation of one style shares one set.
    [[nodiscard]] drawing_set_id annotation_parts(f32 marker_radius, f32 ring_width);

    /// The extent of drawing `index` of the set `id` names, in the drawing's own units.
    [[nodiscard]] tg::aabb2f bounds(drawing_set_id id, u32 index);

    /// The atlas every set lives in; `sr::slug_routine::prepare_job` uploads what acquires placed since the last frame.
    [[nodiscard]] sr::slug_atlas& atlas() { return _atlas; }

private:
    [[nodiscard]] drawing_set_id _place(cc::hash128 hash, cc::span<drawing const> drawings);

    sr::slug_atlas _atlas;
    bool _warned_full = false;
};
