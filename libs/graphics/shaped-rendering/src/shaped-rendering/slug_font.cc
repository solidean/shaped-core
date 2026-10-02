#include <clean-core/common/macros.hh>
#include <clean-core/container/pinned_data.hh>
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/format.hh>
#include <shaped-rendering/slug_font.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/slug_shape.hh>
#include <shaped-rendering/text_layout.hh>

namespace sr
{
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
    auto const laid = layout_text(_face, text, {.size = size});
    auto const unit = laid.scale;
    auto const x_axis = right * unit;
    auto const y_axis = up * unit;

    // The layout runs y down from its box's top, with the first baseline one ascender below it; this places that
    // baseline at `origin`, and every later line further along -`up`.
    auto const first_baseline = f32(_face.metrics().ascender) * unit;
    for (auto const& g : laid.glyphs)
    {
        auto const failed_before = _failures.get_ptr(u16(g.glyph)) != nullptr;
        auto const shape = glyph(g.glyph);
        if (shape.has_error())
        {
            if (!failed_before)
                CC_LOG_WARNING("glyph {} of the font did not compile: {}", u16(g.glyph), shape.error().to_string());
            continue;
        }
        if (!shape.value().is_drawable)
            continue;
        auto const pen = origin + right * g.origin[0] - up * (g.origin[1] - first_baseline);
        out.push_back(make_slug_instance(shape.value(), pen, x_axis, y_axis, srgb_color));
    }
}

f32 slug_font::line_width(cc::string_view text, f32 size) const
{
    return layout_text(_face, text, {.size = size}).box.max[0];
}
} // namespace sr
