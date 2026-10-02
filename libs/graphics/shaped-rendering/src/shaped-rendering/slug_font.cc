#include <clean-core/common/macros.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <shaped-rendering/slug_font.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/slug_shape.hh>

namespace sr
{
namespace
{
/// The code points of UTF-8 `text`, a malformed sequence reading as U+FFFD.
///
/// cc has no UTF-8 code-point iteration yet, only utf8_to_utf16; this belongs there once it does.
template <class Fn>
void for_each_code_point(cc::string_view text, Fn&& fn)
{
    auto i = isize(0);
    while (i < text.size())
    {
        auto const lead = u8(text[i]);
        auto length = 1;
        auto c = u32(lead);
        if (lead >= 0xF0)
        {
            length = 4;
            c = lead & 0x07;
        }
        else if (lead >= 0xE0)
        {
            length = 3;
            c = lead & 0x0F;
        }
        else if (lead >= 0xC0)
        {
            length = 2;
            c = lead & 0x1F;
        }
        else if (lead >= 0x80)
        {
            fn(char32_t(0xFFFD));
            ++i;
            continue;
        }

        if (i + length > text.size())
        {
            fn(char32_t(0xFFFD));
            return;
        }
        for (auto k = 1; k < length; ++k)
            c = (c << 6) | (u8(text[i + k]) & 0x3F);
        fn(char32_t(c));
        i += length;
    }
}

[[nodiscard]] babel::font::glyph_id glyph_of(babel::font::face const& face, char32_t c)
{
    auto const g = face.glyph_for(c);
    return g.has_value() ? g.value() : babel::font::glyph_id::notdef;
}
} // namespace

cc::result<slug_font> slug_font::load(cc::string_view path, i32 face_index)
{
    auto adapter = cc::file_read_stream_adapter::open(path);
    CC_RETURN_IF_ERROR(adapter);
    auto stream = adapter.value().stream();
    auto bytes = stream.read_all();
    CC_RETURN_IF_ERROR(bytes);
    auto face = babel::font::read(cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(bytes).value())), face_index);
    CC_RETURN_IF_ERROR(face);
    if (face.value().outlines() != babel::font::outline_format::truetype)
        return cc::error(cc::format("{} has no TrueType outlines, and CFF ones are not read yet", path));
    return slug_font(cc::move(face).value());
}

cc::result<slug_font> slug_font::load_system_ui_font()
{
    // TrueType-outlined faces only: CFF is not read yet, which rules out most .otf files.
    cc::string_view const candidates[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
    };
    for (auto const path : candidates)
    {
        auto font = load(path);
        if (font.has_value())
            return font;
    }
    return cc::error("no TrueType UI font in any of the places this looks");
}

cc::result<slug_shape_ref> slug_font::glyph(babel::font::glyph_id g)
{
    if (auto const* known = _glyphs.get_ptr(u16(g)))
        return *known;
    if (auto const* failed = _failures.get_ptr(u16(g)))
        return cc::error(*failed);

    auto outline = slug_outline_of(_face, g);
    if (outline.has_error())
    {
        _failures[u16(g)] = outline.error().to_string();
        return cc::error(_failures[u16(g)]);
    }
    auto placed = _atlas.add(compile_slug_shape(outline.value()));
    if (placed.has_error())
    {
        _failures[u16(g)] = placed.error().to_string();
        return cc::error(_failures[u16(g)]);
    }
    _glyphs[u16(g)] = placed.value();
    return placed.value();
}

void slug_font::append_line(cc::vector<slug_instance>& out,
                            cc::string_view text,
                            tg::pos2f origin,
                            f32 size,
                            tg::vec4f srgb_color,
                            tg::vec2f right,
                            tg::vec2f up)
{
    auto const unit = size / f32(_face.units_per_em());
    auto const x_axis = right * unit;
    auto const y_axis = up * unit;
    auto pen = origin;
    for_each_code_point(text,
                        [&](char32_t c)
                        {
                            auto const g = glyph_of(_face, c);
                            auto const failed_before = _failures.get_ptr(u16(g)) != nullptr;
                            auto const shape = glyph(g);
                            if (shape.has_error())
                            {
                                if (!failed_before)
                                    CC_LOG_WARNING("glyph {} of the font did not compile: {}", u16(g),
                                                   shape.error().to_string());
                            }
                            else if (shape.value().is_drawable)
                            {
                                out.push_back(make_slug_instance(shape.value(), pen, x_axis, y_axis, srgb_color));
                            }
                            pen = pen + x_axis * f32(_face.horizontal(g).advance);
                        });
}

f32 slug_font::line_width(cc::string_view text, f32 size) const
{
    auto advance = i64(0);
    for_each_code_point(text, [&](char32_t c) { advance += _face.horizontal(glyph_of(_face, c)).advance; });
    return f32(advance) * size / f32(_face.units_per_em());
}
} // namespace sr
