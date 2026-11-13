#include <clean-core/common/utility.hh> // cc::move
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/thread_bound_scheduler.hh> // cc::async_resume_on
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-rendering/capture.hh> // sr::capture_request
#include <shaped-viewer/context.hh>
#include <shaped-viewer/frame.hh>
#include <shaped-viewer/impl/settled_landing.hh>
#include <shaped-viewer/settled.hh>
#include <shaped-viewer/view/render_settings.hh> // sv::render_channel
#include <shaped-viewer/viewer.hh>

namespace sv
{
cc::shared_async<settled_image> render_settled(sg::context& ctx,
                                               cc::string id,
                                               settled_options options,
                                               cc::unique_function<void(frame&)> author)
{
    if (options.size[0] <= 0 || options.size[1] <= 0)
        co_return cc::error(cc::format("a settled render of {} by {} pixels has none", options.size[0], options.size[1]));

    // a guest runs on its host's home, between the host's frames; an app of its own on the device's home, if any
    if (options.host != nullptr)
        co_await cc::async_resume_on(*options.host);
    else if (auto* const home = ctx.device_home())
        co_await cc::async_resume_on(*home);

    auto created = viewer::try_create(
        ctx, id,
        {.width = options.size[0], .height = options.size[1], .headless = true, .guest = options.host != nullptr});
    if (created.has_error())
        co_return cc::error(cc::move(created.error()));
    auto& v = created.value();

    // The capture's own settling decides when the image is finished; its readback lands here rather than in a file.
    auto landing = sv::impl::settled_landing();
    v.install_settled(
        {.active = true, .size = options.size, .accumulate = options.samples, .timeout_seconds = options.timeout_seconds},
        &landing);
    v.begin_frames();
    while (v.is_running())
    {
        auto& f = v.begin_frame();
        if (f.is_open())
            author(f);
        auto const pacing = v.end_frame_async();
        co_await cc::async_settled(pacing);
    }
    auto const finished = v.finish();
    co_await cc::async_settled(finished);

    if (!landing.readback.has_value())
        co_return cc::error(cc::format("the settled render {} ended before its image was finished", id));
    auto const landed = landing.readback.value().bytes();
    co_await cc::async_settled(landed);
    auto const* const bytes = landed->try_value();
    if (bytes == nullptr)
        co_return cc::error(cc::format("reading the settled render {} back failed", id));

    // The offscreen target is blue, green, red and alpha; a caller gets red first.
    auto const span = bytes->span();
    auto const count = isize(options.size[0]) * options.size[1] * 4;
    if (span.size() < count)
        co_return cc::error(cc::format("the settled render {} read back {} bytes of {}", id, span.size(), count));
    auto out = settled_image{.size = options.size,
                             .rgba = cc::vector<u8>::create_uninitialized(count),
                             .settled = landing.settled};
    for (auto i = isize(0); i < count; i += 4)
    {
        out.rgba[i + 0] = u8(span[i + 2]);
        out.rgba[i + 1] = u8(span[i + 1]);
        out.rgba[i + 2] = u8(span[i + 0]);
        out.rgba[i + 3] = u8(span[i + 3]);
    }

    // Each channel's mean: its sum over the pixel's count of primary samples, an escaped one adding nothing.
    if (landing.channel_sums.has_value())
    {
        auto const sums_landing = landing.channel_sums.value().data();
        co_await cc::async_settled(sums_landing);
        auto const* const sums = sums_landing->try_value();
        if (sums == nullptr)
            co_return cc::error(cc::format("reading the settled render {}'s channels back failed", id));
        auto const pixels = isize(landing.extent[0]) * landing.extent[1];
        auto const slots = isize(cc::popcount(landing.channel_mask)) + 1;
        if (landing.extent != options.size || sums->span().size() < pixels * slots)
            co_return cc::error(cc::format("the settled render {}'s channels are laid out over {} by {}, not its image",
                                           id, landing.extent[0], landing.extent[1]));
        auto k = isize(1);
        for (auto bit = u32(1); bit != 0 && bit <= landing.channel_mask; bit <<= 1)
        {
            if ((landing.channel_mask & bit) == 0)
                continue;
            auto channel = settled_channel{.channel = render_channel(bit),
                                           .values = cc::vector<tg::vec4f>::create_uninitialized(pixels)};
            for (auto s = isize(0); s < pixels; ++s)
            {
                auto const n = sums->span()[s * slots][0];
                channel.values[s] = sums->span()[s * slots + k] / cc::max(n, 1.0f);
            }
            out.channels.push_back(cc::move(channel));
            ++k;
        }
    }
    co_return out;
}

cc::shared_async<settled_image> render_settled(cc::string id,
                                               settled_options options,
                                               cc::unique_function<void(frame&)> author)
{
    // Held for the async, so the context outlives the viewer that renders on it.
    auto handle = acquire_viewer_context();
    if (handle.has_error())
        co_return cc::error(cc::move(handle.error()));
    auto const rendered = render_settled(*handle.value(), cc::move(id), options, cc::move(author));
    co_return co_await cc::async_take(rendered);
}
} // namespace sv
