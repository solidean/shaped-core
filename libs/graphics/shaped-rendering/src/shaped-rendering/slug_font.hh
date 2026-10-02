#pragma once

#include <babel-serializer/font/font.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

/// A font face's glyphs as Slug shapes, compiled into an atlas of its own the first time each is asked for, and a
/// one-line layout over them.
///
/// The layout is deliberately the least that draws a string: each glyph advances by its own advance width, with no
/// kerning, no ligatures and no script shaping.
/// A string with any of those needs the shaping layer that does not exist yet; this is what examples and tests draw with.
class sr::slug_font
{
public:
    explicit slug_font(babel::font::face face) : _face(cc::move(face)) {}

    /// The font file at `path`, face `face_index` of a collection.
    [[nodiscard]] static cc::result<slug_font> load(cc::string_view path, i32 face_index = 0);

    /// A sans-serif TrueType font the operating system ships, for examples and tools with nothing better to draw with.
    /// Fails where none of the usual places holds one; shaped-core vendors no font of its own yet.
    [[nodiscard]] static cc::result<slug_font> load_system_ui_font();

    [[nodiscard]] babel::font::face const& face() const { return _face; }

    /// The atlas every glyph of this font lands in; draw from it after `slug_routine::prepare`.
    [[nodiscard]] slug_atlas& atlas() { return _atlas; }
    [[nodiscard]] slug_atlas const& atlas() const { return _atlas; }

    /// The glyph's place in the atlas, compiling and adding it on first use.
    /// A glyph that fails is remembered, so asking again returns the same error without compiling it again.
    [[nodiscard]] cc::result<slug_shape_ref> glyph(babel::font::glyph_id g);

    /// Appends one instance per visible glyph of the UTF-8 `text`, the baseline starting at `origin`.
    /// `size` is the em height in object units, and the line runs along `right` with glyphs standing up along `up`.
    /// A character the face has no glyph for draws its `.notdef`.
    void append_line(cc::vector<slug_instance>& out,
                     cc::string_view text,
                     tg::pos2f origin,
                     f32 size,
                     tg::vec4f srgb_color,
                     tg::vec2f right = tg::vec2f(1, 0),
                     tg::vec2f up = tg::vec2f(0, 1));

    /// How far `append_line` advances over `text` at `size`.
    [[nodiscard]] f32 line_width(cc::string_view text, f32 size) const;

private:
    babel::font::face _face;
    slug_atlas _atlas;
    cc::map<u16, slug_shape_ref> _glyphs;
    cc::map<u16, cc::string> _failures; ///< why each glyph that did not compile failed
};
