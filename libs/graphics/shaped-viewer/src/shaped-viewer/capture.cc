#include <babel-serializer/image/image.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/platform/environment.hh>
#include <clean-core/streams/file_stream.hh>
#include <clean-core/string/from_string.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/capture.hh>
#include <shaped-viewer/impl/capture_session.hh>

#include <chrono>

namespace sv
{
namespace
{
/// `<width>x<height>`, or nothing when it does not parse.
/// Both halves must be positive: a zero-sized target would fail resource creation with a far less helpful message.
[[nodiscard]] cc::optional<tg::vec2i> parse_size(cc::string_view s)
{
    auto const x = s.find('x');
    if (x < 0)
        return {};

    auto w = 0;
    auto h = 0;
    if (!cc::from_string(s.subview({.offset = 0, .size = x}), w) || !cc::from_string(s.subview(x + 1), h))
        return {};
    if (w <= 0 || h <= 0)
        return {};

    return tg::vec2i(w, h);
}

[[nodiscard]] double now_seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// The container format `path`'s extension names, defaulting to JPEG.
/// The extension is the whole rule: a caller that wants PNG asks for one by naming the file `.png`.
[[nodiscard]] babel::image::format format_of(cc::string_view path)
{
    auto const dot = path.rfind('.');
    if (dot < 0)
        return babel::image::format::jpg;

    auto const ext = path.subview(dot + 1);
    return ext == "png" || ext == "PNG" ? babel::image::format::png : babel::image::format::jpg;
}
} // namespace

capture_request capture_request::from_environment()
{
    auto req = capture_request{};
    req.active = cc::is_environment_flag_set(capture_request_env_var);
    req.list_only = cc::is_environment_flag_set(capture_list_env_var);

    // The listing is a capture run that writes nothing, so it needs the headless bring-up the flag above turns on.
    if (req.list_only)
        req.active = true;

    if (!req.active)
        return req;

    if (auto const name = cc::environment_variable(capture_name_env_var); name.has_value())
        req.name = name.value();

    if (auto const out = cc::environment_variable(capture_output_env_var); out.has_value())
        req.output_path = out.value();

    if (auto const size = cc::environment_variable(capture_size_env_var); size.has_value())
    {
        if (auto const parsed = parse_size(size.value()); parsed.has_value())
            req.size = parsed.value();
        else
            CC_LOG_WARNING("capture: ignoring unparseable {} (want <width>x<height>)", capture_size_env_var);
    }

    if (auto const frames = cc::environment_variable(capture_accumulate_env_var); frames.has_value())
    {
        auto parsed = u32(0);
        if (cc::from_string(frames.value(), parsed) && parsed > 0)
            req.accumulate_frames = parsed;
        else
            CC_LOG_WARNING("capture: ignoring unparseable {}", capture_accumulate_env_var);
    }

    if (auto const timeout = cc::environment_variable(capture_timeout_env_var); timeout.has_value())
    {
        auto parsed = 0.0;
        if (cc::from_string(timeout.value(), parsed) && parsed > 0.0)
            req.timeout_seconds = parsed;
        else
            CC_LOG_WARNING("capture: ignoring unparseable {}", capture_timeout_env_var);
    }

    return req;
}

namespace impl
{
void capture_session::note_registered(cc::string_view name)
{
    for (auto const& n : _registered)
        if (n == name)
            return;

    _registered.push_back(cc::string(name));
}

void capture_session::begin()
{
    _start_seconds = now_seconds();
}

double capture_session::elapsed_seconds() const
{
    return now_seconds() - _start_seconds;
}

bool capture_session::is_out_of_time() const
{
    return elapsed_seconds() >= _request.timeout_seconds;
}

bool capture_session::is_settled(cc::span<u32 const> traced_views, isize pending_work, bool traces_ran) const
{
    // Post-load work changes a texture's contents rather than its id, so it never restarts a view's accumulation.
    // Without this term an image whose mips are still generating settles at full count and looks subtly wrong.
    if (pending_work > 0)
        return false;

    // A trace that never dispatched still lets its accumulator climb, so the frame count alone would accept a black image.
    // Only consulted where there was a trace to run at all.
    if (!traced_views.empty() && !traces_ran)
        return false;

    for (auto const frames : traced_views)
        if (frames < _request.accumulate_frames)
            return false;

    return true;
}

cc::result<cc::unit> write_capture_image(sg::context& ctx,
                                         sg::texture_2d const& texture,
                                         tg::vec2i size,
                                         cc::string_view path)
{
    auto future = ctx.download.bytes_from_texture(texture.raw());
    auto const bytes = ctx.wait_for(future);
    if (!bytes.has_value())
        return cc::error("capture: reading the image back from the GPU failed");

    auto const src = bytes.value().span();
    auto const pixels = isize(size[0]) * isize(size[1]);
    if (src.size() < pixels * 4)
        return cc::error("capture: the readback is smaller than the image it should hold");

    // bgra8_unorm on the GPU, RGB in the file: drop the alpha and swap the two outer channels.
    // The target is opaque — the frame clears it and every view writes over it — so nothing is lost with the alpha.
    auto img = babel::image::image{.width = size[0], .height = size[1], .channels = 3};
    img.pixels.resize_to_uninitialized(pixels * 3);
    for (auto i = isize(0); i < pixels; ++i)
    {
        img.pixels[i * 3 + 0] = src[i * 4 + 2];
        img.pixels[i * 3 + 1] = src[i * 4 + 1];
        img.pixels[i * 3 + 2] = src[i * 4 + 0];
    }

    auto adapter = cc::file_write_stream_adapter::create(path);
    CC_RETURN_IF_ERROR(adapter);

    cc::write_stream out = adapter.value(); // the adapter narrows to the stream babel writes through
    auto written = babel::image::write(out, img, format_of(path), {.jpg_quality = 88});
    CC_RETURN_IF_ERROR(written);

    // The adapter buffers, and its destructor does not drain — so without this the file ends at a 4096-byte boundary,
    // missing whatever was still in the window.
    // A JPEG truncated that way still DECODES, flat-filling the tail from the last DC value, which is why this looks
    // like a rendering artifact rather than a broken file.
    CC_RETURN_IF_ERROR(out.flush());
    return cc::unit{};
}
} // namespace impl
} // namespace sv
