#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <shaped-rendering/slug_font.hh>
#include <shaped-rendering/slug_routine.hh>
#include <shaped-rendering/text_layout.hh>
#include <shaped-viewer/drawing/drawing_manager.hh>
#include <shaped-viewer/drawing/font.hh>
#include <shaped-viewer/drawing/instance.hh>
#include <shaped-viewer/impl/content_hash.hh>

namespace sv
{
cc::result<font> font::from_bytes(cc::pinned_data<byte const> bytes, i32 face_index)
{
    auto const hash = cc::hash128::create(bytes.span(), impl::font_hash_seed + u64(face_index));
    auto face = babel::font::read(cc::move(bytes), face_index);
    CC_RETURN_IF_ERROR(face);
    if (face.value().outlines() != babel::font::outline_format::truetype)
        return cc::error("the font has no TrueType outlines, and CFF ones are not read yet");

    auto f = font();
    f._face = cc::move(face).value();
    f._hash = hash;
    return f;
}

font const* default_font()
{
    // Loaded once for the process: a font is immutable, and every viewer may share it.
    static auto const loaded = []() -> cc::optional<font>
    {
        auto const path = sr::system_ui_font_path();
        if (!path.has_value())
        {
            CC_LOG_WARNING("no system UI font found, so text drawn in the default font draws nothing");
            return {};
        }
        auto adapter = cc::file_read_stream_adapter::open(path.value());
        if (adapter.has_error())
            return {};
        auto stream = adapter.value().stream();
        auto bytes = stream.read_all();
        if (bytes.has_error())
            return {};
        auto f = font::from_bytes(cc::pinned_data<byte const>(cc::make_pinned_data(cc::move(bytes).value())));
        if (f.has_error())
        {
            CC_LOG_WARNING("the system UI font did not load ({}), so text drawn in the default font draws nothing",
                           f.error().to_string());
            return {};
        }
        return cc::move(f).value();
    }();
    return loaded.has_value() ? &loaded.value() : nullptr;
}
} // namespace sv

namespace sv::impl
{
namespace
{
/// How far the box from (0, 0) to `extent` reaches right of and below its origin, placed on the axes.
[[nodiscard]] tg::vec2f reach_of(tg::pos2f extent, tg::vec3f x_axis, tg::vec3f y_axis)
{
    auto reach = tg::vec2f(0, 0);
    for (auto const x : {0.0f, extent[0]})
        for (auto const y : {0.0f, extent[1]})
        {
            auto const o = x_axis * x + y_axis * y;
            reach = tg::vec2f(cc::max(reach[0], o[0]), cc::max(reach[1], o[1]));
        }
    return reach;
}
} // namespace

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
                     sv::corner from)
{
    auto const* const f = style.font != nullptr ? style.font : default_font();
    if (f == nullptr)
        return tg::pos2f(0, 0); // default_font has said why, once

    auto const laid = sr::layout_text(
        f->face(), text,
        {.size = style.size, .line_height = style.line_height, .max_width = style.max_width, .align = style.align});
    auto const color = sr::pack_rgba8(tg::vec4f(style.color[0] * tint[0], style.color[1] * tint[1],
                                                style.color[2] * tint[2], style.color[3] * tint[3]));
    auto const reach = reach_of(laid.box.max, x_axis, y_axis);
    for (auto const& g : laid.glyphs)
    {
        auto const set = drawings.glyph_set(*f, g.glyph);
        auto const index = u32(g.glyph) % drawing_manager::glyphs_per_set;
        auto const count = drawings.record_count(set, index);
        if (count == 0)
            continue; // a space, or a glyph with no outline
        auto const offset = x_axis * g.origin[0] + y_axis * g.origin[1];
        out.push_back({.set = set,
                       .first_record = drawings.first_record(set, index),
                       .record_count = count,
                       .at = at + offset,
                       .x_axis = x_axis * laid.scale,
                       .y_axis = -y_axis * laid.scale,
                       .tint = color,
                       .from = from,
                       .reach = reach,
                       .offset = tg::vec2f(offset[0], offset[1])});
    }
    return laid.box.max;
}
} // namespace sv::impl
