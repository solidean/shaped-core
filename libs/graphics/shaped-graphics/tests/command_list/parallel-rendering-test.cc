#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/native_thread.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>


using namespace cc::primitive_defines;

// Command lists recorded on several threads at once, rendering to the same targets in one epoch.
//
// A rendering scope's attachment descriptors are per command list on dx12 and per texture and epoch on vulkan.
// The second is the one with something to get wrong: concurrent lists race to create a texture's view, every list
// of the epoch shares the one that won, and a dropped list's views must outlive it because the others still use them.
// So each frame here has every thread render to a shared persistent target, a shared transient depth buffer and a
// target of its own, while one thread drops its list on some frames.
//
// The validation layers are the oracle for the lifetimes, and they are on in every driver of this binary.
// The readback is the oracle for every list having rendered through a view of the right image.
//
// Gated on CC_HAS_THREADS, since a single-threaded build has no locking to exercise.

#if CC_HAS_THREADS

namespace
{
constexpr int k_threads = 8;
constexpr int k_frames = 6;
constexpr int k_epochs_in_flight = 2;
constexpr int k_extent = 16;

/// Thread `i`'s clear color: channels 0 or 1 only, so the rgba8 readback is exact.
[[nodiscard]] tg::vec4f color_of(int i)
{
    return tg::vec4f(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1), 1.0f);
}

/// Whether every texel of an rgba8 readback is `color`.
[[nodiscard]] bool all_texels_are(cc::span<byte const> bytes, tg::vec4f color)
{
    if (bytes.size() != isize(k_extent) * k_extent * 4)
        return false;
    for (isize i = 0; i < bytes.size(); ++i)
        if (bytes[i] != byte(color[int(i % 4)] * 255.0f))
            return false;
    return true;
}

/// Whether thread `thread` drops its list in `frame` rather than submitting it — never in the last, which is read back.
[[nodiscard]] bool drops(int thread, int frame)
{
    return thread == 0 && frame % 2 == 1 && frame != k_frames - 1;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - command lists recorded in parallel render to shared targets in one epoch",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (ctx->threading() != sg::thread_model::multi_threaded)
        SKIP("this context records on one thread");

    auto const target_description = sg::texture_2d_description{
        .format = sg::pixel_format::rgba8_unorm,
        .width = k_extent,
        .height = k_extent,
        .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src,
    };
    auto const shared = ctx->persistent.create_texture_2d(target_description);
    auto owned = cc::vector<sg::texture_2d>();
    for (auto i = 0; i < k_threads; ++i)
        owned.push_back(ctx->persistent.create_texture_2d(target_description));
    auto const shared_color = tg::vec4f(0.0f, 1.0f, 1.0f, 1.0f);

    for (auto frame = 0; frame < k_frames; ++frame)
    {
        auto const depth = ctx->transient.create_texture_2d({.format = sg::pixel_format::depth32_float,
                                                             .width = k_extent,
                                                             .height = k_extent,
                                                             .usage = sg::texture_usage::depth_stencil});

        auto threads = cc::vector<cc::native_thread>::create_with_capacity(k_threads);
        cc::atomic<int> ready = 0;
        cc::atomic<int> failed_creates = 0;
        for (auto i = 0; i < k_threads; ++i)
            threads.push_back(cc::native_thread::start(
                [&, i]
                {
                    auto cmd = ctx->create_command_list();
                    if (cmd == nullptr)
                    {
                        ++failed_creates;
                        return;
                    }

                    // Lined up, so the threads reach the shared targets' first view of the epoch together.
                    ++ready;
                    while (ready.load() < k_threads)
                        cc::this_thread_yield();

                    {
                        auto pass = cmd->raster.render_to(
                            {.color_targets = {shared.as_render_target_view().cleared(shared_color)},
                             .depth_stencil_target = depth.as_depth_stencil_view().cleared(1.0f)});
                    }
                    {
                        auto pass = cmd->raster.render_to(
                            {.color_targets = {owned[i].as_render_target_view().cleared(color_of(i))},
                             .depth_stencil_target = depth.as_depth_stencil_view().preserved()});
                    }

                    if (drops(i, frame))
                        ctx->drop_command_list(cc::move(cmd));
                    else
                        (void)ctx->submit_command_list(cc::move(cmd));
                }));
        for (auto& t : threads)
            t.join();
        REQUIRE(failed_creates.load() == 0).context(cc::format("frame {}", frame));

        if (frame == k_frames - 1)
        {
            auto readback = ctx->create_command_list();
            REQUIRE(readback != nullptr);
            auto const shared_bytes = readback->download.bytes_from_texture(shared.raw());
            auto owned_bytes = cc::vector<sg::bytes_future>();
            for (auto const& t : owned)
                owned_bytes.push_back(readback->download.bytes_from_texture(t.raw()));
            (void)ctx->submit_command_list(cc::move(readback));

            CHECK(all_texels_are(co_await shared_bytes.bytes(), shared_color));
            for (auto i = 0; i < k_threads; ++i)
                CHECK(all_texels_are(co_await owned_bytes[i].bytes(), color_of(i))).context(cc::format("thread {}", i));
        }

        ctx->advance_epoch();
        co_await ctx->epochs_in_flight_completion(k_epochs_in_flight);
    }

    co_await ctx->idle_completion();
    CHECK(!ctx->is_device_lost());
}

#endif // CC_HAS_THREADS
