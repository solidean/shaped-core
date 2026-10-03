#pragma once

#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

/// Where an atlas placed a shape, and everything an instance needs to draw it.
struct sr::slug_shape_ref
{
    /// The shape's band header in the band texture, as `x | y << 16`.
    u32 glyph_location = 0;

    /// The last vertical band's index, and above it the last horizontal band's index and the fill-rule flag:
    /// `band max x | (band max y | flags) << 16`, the reference's packing.
    u32 band_info = 0;

    tg::vec4f banding;

    /// The shape's box in its stored space; an instance maps this to the object.
    tg::aabb2f em_bounds;

    /// Stored units per outline unit.
    f32 em_scale = 1.0f;

    /// The outline point stored (0, 0) sits at, in outline units.
    tg::pos2f stored_origin = tg::pos2f(0, 0);

    /// The packed shape data module `slug`'s `coverage` takes as its `glyph` argument:
    /// the band header's x and y, the last vertical band, and the last horizontal band with the flags above it.
    [[nodiscard]] tg::vec4i glyph() const
    {
        return tg::vec4i(i32(glyph_location & 0xffff), i32(glyph_location >> 16), i32(band_info & 0xffff),
                         i32(band_info >> 16));
    }

    /// False for an empty shape — a space — which the caller skips rather than make an instance of.
    bool is_drawable = false;
};

/// The two textures Slug covers shapes from, and where every shape added to them sits.
///
/// Caller-owned: an atlas lives as long as the draws that read it, and several can coexist — one per font, per document.
/// Shapes are added on the CPU, and `prepare` creates, grows and uploads the textures on a command list.
/// Append-only: nothing is ever evicted, so an atlas holding a large font grows by every glyph it is asked for.
/// A caller that must reclaim space keeps several atlases capped at a few rows each, and replaces a whole one.
///
/// A third texture holds **records**: shape instances kept beside the shapes they name, which a job draw
/// (`slug_routine::prepare_job`) places many times through frames without uploading them again.
///
/// Holds no device until the first `prepare`, which is what lets a test add shapes and read back what it placed.
class sr::slug_atlas
{
public:
    /// Every texture is this many texels wide; the reference's shader wraps band lists at this width.
    static constexpr int width = 4096;

    /// The tallest a texture grows to, which is WebGPU's floor for a 2D texture's dimension.
    static constexpr int max_rows = 8192;

    /// Texels one record takes in the record texture, and how many records a row holds.
    static constexpr int texels_per_record = 5;
    static constexpr int records_per_row = width / texels_per_record;

    slug_atlas() = default;

    /// An atlas whose textures stop at `row_limit` rows, at most `max_rows`: `add` fails once a shape would need more.
    explicit slug_atlas(int row_limit);
    slug_atlas(slug_atlas&&) noexcept = default;
    slug_atlas& operator=(slug_atlas&&) noexcept = default;
    slug_atlas(slug_atlas const&) = delete;
    slug_atlas& operator=(slug_atlas const&) = delete;

    /// Places `shape`, on the CPU only; the textures see it after the next `prepare`.
    /// An empty shape places nothing and returns a ref that is not drawable.
    /// Fails when the atlas is full, or a shape's band data does not fit one row.
    [[nodiscard]] cc::result<slug_shape_ref> add(slug_compiled_shape const& shape);

    /// Keeps `records` as consecutive records, on the CPU only, and returns the index of the first.
    /// Every record must name a shape of this atlas; a job's quad names a record by its index.
    /// Fails when the record texture is full.
    [[nodiscard]] cc::result<u32> add_records(cc::span<slug_instance const> records);

    /// How many records `add_records` has kept.
    [[nodiscard]] isize record_count() const { return _record_count; }

    /// The rows each texture may grow to.
    [[nodiscard]] int row_limit() const { return _row_limit; }

    /// Creates the textures, regrows them, and uploads what `add` and `add_records` placed since the last call.
    /// Records copies, so it must be called before the rendering scope that draws from this atlas opens.
    void prepare(sg::command_list& cmd);

    /// Whether `add` or `add_records` placed something `prepare` has not uploaded yet.
    [[nodiscard]] bool has_pending_upload() const
    {
        return _curve_dirty_row < _curve_rows || _band_dirty_row < _band_rows || _record_dirty_row < _record_rows;
    }

    /// The textures as of the last `prepare`; null before the first, and the record texture null until a record exists.
    [[nodiscard]] sg::texture_2d const& curve_texture() const { return _curve_texture; }
    [[nodiscard]] sg::texture_2d const& band_texture() const { return _band_texture; }
    [[nodiscard]] sg::texture_2d const& record_texture() const { return _record_texture; }

    /// The CPU copies, `width` texels a row: half-float bits for curves, two u16 for bands.
    [[nodiscard]] cc::span<cc::fixed_array<u16, 4> const> curve_texels() const { return _curve_texels; }
    [[nodiscard]] cc::span<cc::fixed_array<u16, 2> const> band_texels() const { return _band_texels; }

    /// The record kept at `index`, read back from the CPU copy; `index` must be below `record_count`.
    [[nodiscard]] slug_instance record(u32 index) const;

private:
    int _row_limit = max_rows;
    cc::vector<cc::fixed_array<u16, 4>> _curve_texels;
    cc::vector<cc::fixed_array<u16, 2>> _band_texels;
    cc::vector<cc::fixed_array<u32, 4>> _record_texels;
    isize _record_count = 0;
    int _record_rows = 0;
    int _record_dirty_row = 0;
    sg::texture_2d _record_texture;

    // The next free texel of each texture, and how many rows hold anything.
    tg::pos2i _curve_cursor = tg::pos2i(0, 0);
    tg::pos2i _band_cursor = tg::pos2i(0, 0);
    int _curve_rows = 0;
    int _band_rows = 0;

    // The first row `prepare` still has to upload.
    int _curve_dirty_row = 0;
    int _band_dirty_row = 0;

    sg::texture_2d _curve_texture;
    sg::texture_2d _band_texture;
};
