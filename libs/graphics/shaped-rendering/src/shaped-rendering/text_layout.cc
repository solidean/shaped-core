#include <clean-core/common/utility.hh> // cc::max
#include <clean-core/string/conversion.hh>
#include <shaped-rendering/text_layout.hh>

namespace sr
{
namespace
{
/// One glyph of a line before alignment: its glyph and its pen position, in font units.
struct pen_glyph
{
    babel::font::glyph_id glyph = babel::font::glyph_id::notdef;
    i64 x = 0;
};

/// A line's glyphs and the advance it ends at, in font units.
struct line
{
    cc::vector<pen_glyph> glyphs;
    i64 width = 0;
};

[[nodiscard]] babel::font::glyph_id glyph_of(babel::font::face const& face, char32_t c)
{
    auto const g = face.glyph_for(c);
    return g.has_value() ? g.value() : babel::font::glyph_id::notdef;
}

/// Sets one paragraph — text with no line break — into one or more lines, wrapping at spaces past `max_units`.
void set_paragraph(babel::font::face const& face, cc::span<char32_t const> text, i64 max_units, cc::vector<line>& out)
{
    auto current = line();
    auto previous = cc::optional<babel::font::glyph_id>();

    // Where the last space of the current line was, so a wrap can cut there: the glyph index after it, and the width
    // the line had before it.
    auto break_glyph = isize(-1);
    auto break_width = i64(0);

    for (auto const c : text)
    {
        auto const g = glyph_of(face, c);
        auto kern = previous.has_value() ? i64(face.pair_kerning(previous.value(), g)) : 0;
        auto const advance = i64(face.horizontal(g).advance);
        auto const is_space = c == U' ' || c == U'	';

        // A glyph that would cross the limit after a space on this line wraps the line at that space, taking the word
        // started since then along.
        if (max_units > 0 && !is_space && break_glyph >= 0 && current.width + kern + advance > max_units)
        {
            auto next = line();
            auto const shift = break_glyph < current.glyphs.size() ? current.glyphs[break_glyph].x : current.width;
            for (auto i = break_glyph; i < current.glyphs.size(); ++i)
                next.glyphs.push_back({.glyph = current.glyphs[i].glyph, .x = current.glyphs[i].x - shift});
            next.width = current.width - shift;
            current.glyphs.resize_to_defaulted(break_glyph);
            current.width = break_width;
            out.push_back(cc::move(current));
            current = cc::move(next);
            break_glyph = -1;

            // Nothing on the new line before this glyph means nothing to kern against.
            if (current.glyphs.empty())
                kern = 0;
        }

        if (is_space)
        {
            // The space stays on the line it ends; what follows it starts the next one if the line wraps there.
            current.glyphs.push_back({.glyph = g, .x = current.width + kern});
            break_width = current.width;
            current.width += kern + advance;
            break_glyph = current.glyphs.size();
            previous = g;
            continue;
        }

        current.glyphs.push_back({.glyph = g, .x = current.width + kern});
        current.width += kern + advance;
        previous = g;
    }
    out.push_back(cc::move(current));
}
} // namespace

text_layout layout_text(babel::font::face const& face, cc::string_view text, text_style const& style)
{
    auto const& m = face.metrics();
    auto const scale = style.size / f32(face.units_per_em());
    auto const max_units = style.max_width > 0.0f ? i64(style.max_width / scale) : i64(0);

    auto lines = cc::vector<line>();
    auto const codepoints = cc::utf8_to_utf32(text);
    auto paragraph_start = isize(0);
    for (auto i = isize(0); i <= codepoints.size(); ++i)
    {
        if (i < codepoints.size() && codepoints[i] != U'\n')
            continue;
        auto paragraph
            = cc::span<char32_t const>(codepoints).subspan({.offset = paragraph_start, .size = i - paragraph_start});
        // A carriage return before the line break is the line break's, not a glyph.
        if (!paragraph.empty() && paragraph[paragraph.size() - 1] == U'\r')
            paragraph = paragraph.subspan({.offset = 0, .size = paragraph.size() - 1});
        set_paragraph(face, paragraph, max_units, lines);
        paragraph_start = i + 1;
    }

    auto widest = i64(0);
    for (auto const& l : lines)
        widest = cc::max(widest, l.width);
    auto const box_width = style.max_width > 0.0f ? style.max_width : f32(widest) * scale;

    auto const ascender = f32(m.ascender) * scale;
    auto const descender = f32(m.descender) * scale;
    auto const spacing = f32(m.ascender - m.descender + m.line_gap) * scale * style.line_height;

    auto out = text_layout{.scale = scale};
    out.line_count = i32(lines.size());
    for (auto li = isize(0); li < lines.size(); ++li)
    {
        auto const& l = lines[li];
        auto const slack = box_width - f32(l.width) * scale;
        auto const indent = style.align == text_align::center ? slack * 0.5f
                          : style.align == text_align::right  ? slack
                                                              : 0.0f;
        auto const baseline = ascender + f32(li) * spacing;
        for (auto const& g : l.glyphs)
            out.glyphs.push_back({.glyph = g.glyph, .origin = tg::pos2f(indent + f32(g.x) * scale, baseline)});
    }
    out.box = tg::aabb2f(tg::pos2f(0, 0),
                         tg::pos2f(box_width, ascender + f32(cc::max(isize(0), lines.size() - 1)) * spacing - descender));
    return out;
}
} // namespace sr
