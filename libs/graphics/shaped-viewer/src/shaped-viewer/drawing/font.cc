#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/record/log.hh>
#include <clean-core/streams/file_stream.hh>
#include <shaped-rendering/slug_font.hh>
#include <shaped-viewer/drawing/font.hh>
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
