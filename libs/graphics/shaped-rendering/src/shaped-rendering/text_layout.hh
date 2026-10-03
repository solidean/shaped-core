#pragma once

#include <babel-serializer/font/font.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>
#include <typed-geometry/linalg/pos.hh>

// Text layout: a string set in one face, as positioned glyphs and the box they fill.
// Advances, pair kerning, line breaks and wrapping at spaces, aligned left, centered or right.
// No ligatures, no mark positioning and no reordering, so scripts that need shaping — Arabic, Hebrew, Indic — come out
// as their glyphs in logical order; libs/graphics/shaped-rendering/docs/slug.md names that as the next layer.

/// Where each line sits within the laid-out box.
enum class sr::text_align : sr::u8
{
    left,
    center,
    right,
};

/// How a string is set.
struct sr::text_style
{
    /// The em height, in whatever units the layout is used in.
    f32 size = 1.0f;

    /// The distance between baselines, as a multiple of the face's own line spacing (ascender - descender + line gap).
    f32 line_height = 1.0f;

    /// Wraps at spaces so no line is wider than this; 0 never wraps.
    /// A word wider on its own than this is not split, and overhangs.
    f32 max_width = 0.0f;

    text_align align = text_align::left;
};

/// One glyph of laid-out text: which glyph, and where its origin — on the baseline, at its left edge — sits.
struct sr::laid_out_glyph
{
    babel::font::glyph_id glyph = babel::font::glyph_id::notdef;
    tg::pos2f origin;
};

/// A string set in a face.
///
/// Positions are y DOWN from the top-left of `box`, which is what a 2D drawing's own coordinates are: the first
/// baseline sits one ascender below the top, and every later one a line spacing further down.
/// A glyph's outline is y UP in font units, so whoever places one maps its y to -y.
struct sr::text_layout
{
    /// Every glyph but line breaks, spaces included, in reading order.
    cc::vector<laid_out_glyph> glyphs;

    /// From the top-left corner to the widest line's right edge — or `max_width`, when the style wraps — and down to the
    /// last line's descender.
    tg::aabb2f box;

    i32 line_count = 0;

    /// Font units to layout units: `size / units_per_em`.
    f32 scale = 1.0f;
};

namespace sr
{
/// Sets the UTF-8 `text` in `face`: one line per `\n`, kerned pair by pair, wrapped and aligned as `style` says.
/// A character the face has no glyph for is set as its `.notdef`.
[[nodiscard]] text_layout layout_text(babel::font::face const& face, cc::string_view text, text_style const& style);
} // namespace sr
