#pragma once

#include <babel-serializer/fwd.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh> // cc::read_stream
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>

// OpenType / TrueType font reader (font/).
//
// A plain data reader: each table is reported as the file stores it, and nothing is converted into another shape.
// A TrueType outline is its points with their on-curve flags, in font units, exactly as `glyf` lists them.
// Implied on-curve midpoints, composite flattening and curve conversion are the caller's — Slug's glyph compilation
// in shaped-rendering is the first.
//
// The face keeps the file's bytes and decodes a glyph only when it is asked for, so a font of ten thousand glyphs
// costs its table directory up front and nothing else.
// That is why it takes bytes rather than a stream: every query is random access into the whole file.
//
//   auto const f = babel::font::read(bytes).value();
//   auto const g = f.glyph_for(U'A');
//   auto const outline = f.outline(g.value()).value();

/// A glyph's index in its face.
/// Glyph 0 is `.notdef`, the glyph a face draws for a character it has none for, and it is an ordinary glyph.
enum class babel::font::glyph_id : babel::u16
{
    notdef = 0,
};

/// Which outline table a face carries.
enum class babel::font::outline_format : babel::u8
{
    /// `glyf`: quadratic contours, decoded by `face::outline`.
    truetype,

    /// `CFF ` or `CFF2`: cubic charstrings, which this reader does not decode yet.
    cff,

    /// Neither table: a bitmap-only or otherwise unsupported face.
    none,
};

/// The face-wide numbers, all in font units except `units_per_em`, which is how many of them one em holds.
struct babel::font::face_metrics
{
    i32 units_per_em = 0;
    i32 glyph_count = 0;

    /// `hhea`'s line metrics; `descender` is negative below the baseline, as the file stores it.
    i32 ascender = 0;
    i32 descender = 0;
    i32 line_gap = 0;

    /// `head`'s box around every glyph of the face.
    tg::aabb2i bounds;
};

/// One glyph's `hmtx` entry.
struct babel::font::horizontal_metric
{
    i32 advance = 0;
    i32 left_side_bearing = 0;
};

/// One point of a simple `glyf` outline.
struct babel::font::glyf_point
{
    tg::pos2i position;

    /// Off-curve points are quadratic control points; two in a row imply an on-curve point midway between them.
    bool on_curve = true;
};

/// One reference of a composite `glyf` outline to another glyph, with its placement as stored.
struct babel::font::glyf_component
{
    glyph_id glyph = glyph_id::notdef;

    /// The component's flags word, as stored; the decoded fields below already account for the layout bits.
    u16 flags = 0;

    /// When true, `arg1` / `arg2` are an x / y offset in font units.
    /// When false they are point indices: point `arg2` of the component lands on point `arg1` of the glyph so far.
    bool args_are_offset = true;
    i32 arg1 = 0;
    i32 arg2 = 0;

    /// The 2x2 linear part, decoded from F2Dot14; identity when the component states none.
    /// Applied as x' = xx * x + yx * y, y' = xy * x + yy * y, the order the spec lists them in.
    f32 xx = 1.0f;
    f32 xy = 0.0f;
    f32 yx = 0.0f;
    f32 yy = 1.0f;
};

/// A glyph's `glyf` record: either a simple outline or a list of components, never both.
/// An empty record — a space — has neither and no bounds worth reading.
struct babel::font::glyf_outline
{
    /// The glyph header's box, in font units.
    tg::aabb2i bounds;

    /// Simple glyphs: every point, contour after contour.
    cc::vector<glyf_point> points;

    /// Simple glyphs: the index of each contour's last point, ascending.
    cc::vector<i32> contour_ends;

    /// Composite glyphs: the components, in file order.
    cc::vector<glyf_component> components;

    [[nodiscard]] bool is_composite() const { return !components.empty(); }
    [[nodiscard]] bool is_empty() const { return points.empty() && components.empty(); }
};

/// One face of a font file, holding the file's bytes and its table directory.
/// Copying shares the bytes and copies the kerning index.
class babel::font::face
{
public:
    [[nodiscard]] face_metrics const& metrics() const { return _metrics; }
    [[nodiscard]] i32 glyph_count() const { return _metrics.glyph_count; }
    [[nodiscard]] i32 units_per_em() const { return _metrics.units_per_em; }
    [[nodiscard]] outline_format outlines() const { return _outlines; }

    /// The glyph `cmap` maps a Unicode scalar value to, or nullopt when the face has none for it.
    /// A face whose map sends the character to glyph 0 has none for it either.
    [[nodiscard]] cc::optional<glyph_id> glyph_for(char32_t codepoint) const;

    /// The glyph's `hmtx` entry; a glyph past the long metrics shares the last advance, as the format says.
    /// `g` must be below `glyph_count()`.
    [[nodiscard]] horizontal_metric horizontal(glyph_id g) const;

    /// Decodes the glyph's `glyf` record.
    /// Fails on a record running past its table, or on a face whose outlines are not `glyf`.
    /// `g` must be below `glyph_count()`.
    [[nodiscard]] cc::result<glyf_outline> outline(glyph_id g) const;

    /// How much the advance after `left` changes when `right` follows it, in font units; 0 for an unkerned pair.
    ///
    /// A query over the tables as stored, as `glyph_for` is over `cmap`: the pair adjustments of the `GPOS` lookups a
    /// `kern` feature names, both the per-pair and the per-class form, summed over lookups and through extension lookups.
    /// Only one language system's `kern` features apply: `DFLT`'s, else `latn`'s, else the first script's.
    /// A face whose script list is empty applies every `kern` feature instead.
    /// Only the first glyph's X advance is read: no placement, no second glyph's value, no device tables.
    /// A face with no such lookups answers from the legacy `kern` table, summed over its horizontal subtables.
    /// A malformed table never fails the query: what cannot be read adds nothing, since kerning only refines spacing.
    [[nodiscard]] i32 pair_kerning(glyph_id left, glyph_id right) const;

private:
    friend cc::result<face> read(cc::pinned_data<byte const> bytes, i32 face_index);

    cc::pinned_data<byte const> _bytes;
    face_metrics _metrics;
    outline_format _outlines = outline_format::none;

    // Offsets and sizes of the tables the queries read, validated to lie inside the file.
    cc::span<byte const> _glyf;
    cc::span<byte const> _loca;
    cc::span<byte const> _hmtx;
    cc::span<byte const> _cmap_subtable;

    /// A `GPOS` pair-adjustment subtable of a `kern` feature's lookup, extensions resolved, and the lookup it is in.
    struct pair_subtable
    {
        cc::span<byte const> bytes;
        i32 lookup = 0;
    };

    // The pair subtables in lookup order; and the legacy `kern` table, read only when there are none.
    cc::vector<pair_subtable> _pair_subtables;
    cc::span<byte const> _kern;
    i32 _cmap_format = 0;
    i32 _long_metrics = 0;
    bool _long_loca = false;
};

namespace babel::font
{
/// Parses the table directory of face `face_index` of a font file: a `.ttf` / `.otf` has one face, a `.ttc` several.
/// The returned face shares `bytes`, which nothing copies.
/// Fails on a missing required table (`head`, `maxp`, `hhea`, `hmtx`, `cmap`), one that runs past the file, or a face
/// with no glyphs, which has no `.notdef` to fall back on.
[[nodiscard]] cc::result<face> read(cc::pinned_data<byte const> bytes, i32 face_index = 0);

/// Convenience: COPIES the bytes into an owned pin, since the face keeps them.
[[nodiscard]] cc::result<face> read(cc::span<byte const> bytes, i32 face_index = 0);

/// Convenience: slurps the stream, then pins the slurped buffer without copying it again.
[[nodiscard]] cc::result<face> read(cc::read_stream& in, i32 face_index = 0);
} // namespace babel::font
