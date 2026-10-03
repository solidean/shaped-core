#pragma once

#include <babel-serializer/font/font.hh>
#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/error/result.hh>
#include <shaped-rendering/text_layout.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// A font, as the content its text is drawn from: a TrueType face and the hash of its file.
///
/// A value like `sv::mesh`: cheap to copy, since the bytes are shared, and keyed by content, so two loads of one file
/// are one font and its glyphs are compiled once.
/// Its glyphs reach the GPU as drawing sets of a fixed 64 each, compiled the first time text needs one of them.
class sv::font
{
public:
    /// The font in `bytes`, face `face_index` of a collection.
    /// Fails on a file babel cannot read, or one whose outlines are not TrueType — CFF is not read yet.
    [[nodiscard]] static cc::result<font> from_bytes(cc::pinned_data<byte const> bytes, i32 face_index = 0);

    [[nodiscard]] babel::font::face const& face() const { return _face; }
    [[nodiscard]] cc::hash128 hash() const { return _hash; }

private:
    babel::font::face _face;
    cc::hash128 _hash;
};

/// How a string is drawn: the font, the size and layout, and the color.
struct sv::text_style
{
    /// The font, or null for `sv::default_font()`; it must outlive the call that draws with it.
    sv::font const* font = nullptr;

    /// The em height: logical pixels on a canvas, world units in a scene.
    f32 size = 14.0f;

    /// Baseline spacing as a multiple of the font's own; wrapping width in the same units as `size`, 0 never wraps.
    f32 line_height = 1.2f;
    f32 max_width = 0.0f;
    sr::text_align align = sr::text_align::left;

    /// Straight alpha, sRGB-encoded, in [0, 1].
    tg::vec4f color = tg::vec4f(1, 1, 1, 1);
};

namespace sv
{
/// The font text is drawn in when its style names none: a sans-serif TrueType font the operating system ships.
/// Loaded once, the first time it is asked for; null where none of the usual places holds one, and text then draws
/// nothing, having said so once.
/// shaped-core ships no font of its own, so what this is differs between operating systems, and captures with it.
[[nodiscard]] font const* default_font();
} // namespace sv

namespace sv::impl
{
/// Sets `text` in `style` and appends a placement per visible glyph to `out`, returning the laid-out box's extent.
/// The layout's own coordinates, y down, land at `at + u * x_axis + v * y_axis`; a glyph's outline is y up, so its
/// drawing is placed with the y axis negated.
tg::pos2f place_text(drawing_manager& drawings,
                     cc::vector<drawing_placement>& out,
                     cc::string_view text,
                     text_style const& style,
                     tg::pos3f at,
                     tg::vec3f x_axis,
                     tg::vec3f y_axis,
                     tg::vec4f tint,
                     sv::corner from);
} // namespace sv::impl
