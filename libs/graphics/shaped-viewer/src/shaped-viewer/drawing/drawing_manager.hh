#pragma once

#include <babel-serializer/font/font.hh>
#include <clean-core/container/vector.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/impl/lru_pool.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>

/// One acquired drawing set: the atlas page it lives in, and where each of its drawings' records sits there.
/// A layer too large for any atlas is skipped, so its drawing draws without it.
/// A drawing whose records did not fit has a count of 0, and draws nothing.
struct sv::drawing_set_record
{
    u32 page = 0;
    cc::vector<u32> first_record;
    cc::vector<u32> record_count;

    /// Each drawing's extent in its own units, from its outlines' points; empty for a drawing with nothing in it.
    cc::vector<tg::aabb2f> bounds;

    /// Whether the set found room; a decal set that found the decal atlas full of this frame's decals did not.
    bool placed = true;
};

/// Hands out `drawing_set_id`s and owns the Slug atlas pages every acquired set's shapes and records live in.
///
/// The drawing counterpart of `mesh_manager`: an acquire is a content-hash lookup, and a miss compiles the set's outlines
/// on the CPU and places them, to be uploaded by `prepare_job` before the frame's first pass.
///
/// **A set lives in one page, and space is reclaimed a page at a time.**
/// A page is an atlas capped at `page_rows` rows; a set that does not fit the newest one opens another.
/// Once there are `max_pages`, the next opens by emptying the page least recently drawn from, and its sets are placed
/// again the next time anything acquires them.
/// A page something drew from this frame is never emptied, since this frame's placements name its records; when every
/// page is in use the manager grows past the limit rather than draw something wrong.
class sv::drawing_manager : public impl::lru_pool<drawing_set_id, drawing_set_record>
{
public:
    /// Rows each page's textures may grow to, and pages kept before the least recently used one is emptied.
    /// A full page is 4096 x `page_rows` texels each of curves (8 bytes), bands (4) and records (16): about 56 MiB at the
    /// defaults, so about 224 MiB for four, plus the decal atlas and the CPU copy every atlas keeps.
    explicit drawing_manager(int page_rows = 512, isize max_pages = 4);

    /// Starts frame `e`: pages drawn from before it become candidates to empty again.
    void begin_frame(sg::epoch e);

    /// The id for `set`, resident from a prior acquire (O(1) through the set's cache slot), or freshly placed.
    [[nodiscard]] drawing_set_id acquire(drawing_set const& set);

    /// The id for a lone drawing: the same as for a one-element set holding it, whose records it would have.
    [[nodiscard]] drawing_set_id acquire(drawing const& d);

    /// The record range of drawing `index` of the set `id` names, which must be resident.
    /// Asking marks the set's page drawn from this frame, which is what keeps it from being emptied under the frame.
    [[nodiscard]] u32 first_record(drawing_set_id id, u32 index);
    [[nodiscard]] u32 record_count(drawing_set_id id, u32 index);

    /// Glyphs per glyph set: a font's glyphs reach the GPU in fixed sets of this many consecutive glyph ids.
    static constexpr u32 glyphs_per_set = 64;

    /// The set holding glyph `g` of `f` and its neighbors, each glyph a drawing of its outline in font units, y up.
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

    /// The page the set `id` names lives in, which must be resident.
    [[nodiscard]] u32 page_of(drawing_set_id id);

    /// Page `page`'s atlas; `sr::slug_routine::prepare_job` uploads what acquires placed since the last frame.
    [[nodiscard]] sr::slug_atlas& atlas(u32 page) { return _pages[isize(page)].atlas; }
    [[nodiscard]] isize page_count() const { return _pages.size(); }

    /// The id for `set` placed in the decal atlas rather than a page, keyed apart from the same set drawn as an instance.
    /// **Every decal lives in one atlas**, since a trace binds one: when a set does not fit it, the atlas is emptied
    /// and its sets placed again as they are next acquired, unless a decal drew from it this frame.
    /// Then the set draws nothing this frame, and is placed again on a later one once the atlas can be emptied for it.
    [[nodiscard]] drawing_set_id acquire_decal(drawing_set const& set);

    /// The same for a lone drawing.
    [[nodiscard]] drawing_set_id acquire_decal(drawing const& d);

    /// The atlas every decal's shapes and records live in; whoever traces with it prepares it.
    [[nodiscard]] sr::slug_atlas& decal_atlas() { return _decals.atlas; }

    /// What `page_of` answers for a set in the decal atlas.
    static constexpr u32 decal_page = u32(-1);

private:
    struct page
    {
        sr::slug_atlas atlas;
        cc::vector<drawing_set_id> sets;
        sg::epoch last_used = sg::epoch(0);
    };

    [[nodiscard]] page& _page(u32 index) { return index == decal_page ? _decals : _pages[isize(index)]; }

    [[nodiscard]] drawing_set_id _place(cc::hash128 hash, cc::span<drawing const> drawings);
    [[nodiscard]] drawing_set_id _acquire_decal(cc::hash128 hash, cc::span<drawing const> drawings);
    [[nodiscard]] drawing_set_id _place_decal(cc::hash128 hash, cc::span<drawing const> drawings);

    /// A record of `count` drawings that draw nothing, for a set that fits nowhere.
    [[nodiscard]] static drawing_set_record _empty_record(isize count);

    /// Places every drawing of a set in `p`, or reports that one did not fit.
    [[nodiscard]] bool _try_place(page& p, cc::span<drawing const> drawings, drawing_set_record& out, isize& bytes);

    /// A page with nothing in it: a new one while under the limit, else the least recently used one emptied.
    [[nodiscard]] u32 _fresh_page();

    cc::vector<page> _pages;
    page _decals;
    u32 _current = 0;
    int _page_rows = 512;
    isize _max_pages = 4;
    sg::epoch _epoch = sg::epoch(0);
    bool _warned_full = false;
    bool _warned_over = false;
    bool _warned_decals_full = false;
};
