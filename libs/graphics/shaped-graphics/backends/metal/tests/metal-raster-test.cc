#include "metal-test-common.hh"
#include "triangle.metallib.h"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>

#include <atomic>
#include <thread>

// Raster, end to end: a pipeline built from a metallib, a rendering scope that clears and draws, and the target read
// back.
//
// The tier-1 suite has no raster execution test, for the same reason it has no compute one — bytecode is per-backend —
// so this tier is the specification for the rendering scope and the draw.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

namespace
{
[[nodiscard]] sg::compiled_shader triangle_stage(sg::shader_stage stage, cc::string entry)
{
    auto shader = sg::compiled_shader{};
    shader.stage = stage;
    shader.format = sg::shader_format::metal_lib;
    shader.entry_point = cc::move(entry);

    auto blob = cc::pinned_data<byte>::create_uninitialized(isize(sizeof(mtl::test::triangle_metallib)));
    cc::memcpy(blob.data(), mtl::test::triangle_metallib, sizeof(mtl::test::triangle_metallib));
    shader.bytecode = cc::pinned_data<byte const>(cc::move(blob));
    return shader;
}

[[nodiscard]] cc::result<sg::backend::metal::metal_raster_pipeline_handle> make_triangle_pipeline(
    mtl::metal_context_handle const& ctx,
    sg::pixel_format format)
{
    auto layout = ctx->create_metal_pipeline_layout({}, sg::lifetime_scope::persistent);
    if (layout.has_error())
        return cc::error(layout.error().to_string());

    auto desc = sg::raster_pipeline_description{
        .layout = layout.value(),
        .vertex_shader = triangle_stage(sg::shader_stage::vertex, "vertex_main"),
        .fragment_shader = triangle_stage(sg::shader_stage::fragment, "fragment_main"),
        // No culling: what this fixture exercises is the rendering scope and the draw, and sg's default cull_mode is
        // `back` — so a winding the fixture never meant to assert would otherwise decide whether anything appears.
        .rasterization = {.cull = sg::cull_mode::none},
    };
    desc.color_targets.push_back({.format = format});

    return ctx->create_metal_raster_pipeline(desc, sg::lifetime_scope::persistent);
}
} // namespace

TEST("sg metal - a raster pipeline builds from a metal library")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto pipeline = make_triangle_pipeline(ctx, sg::pixel_format::rgba8_unorm);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    CHECK(pipeline.value()->state() != nullptr);

    // The depth and stencil test is a separate object here, bound on the encoder rather than baked into the pipeline —
    // so it exists even for a pipeline with no depth attachment at all.
    CHECK(pipeline.value()->depth_stencil_state() != nullptr);
}

TEST("sg metal - pipelines build concurrently from several contexts")
{
    auto const probe = mtl::test::make_context();
    if (probe == nullptr)
        SKIP("no metal 4 device on this host");

    // **Concurrent MTL4 pipeline compilation aborts inside the driver**, in `_os_unfair_lock_corruption_abort` under
    // `AGXG16GFamilyCompiler`, and it is the driver rather than the validation layer — it reproduces with
    // MTL_DEBUG_LAYER=0.
    // So the backend takes every compile under one process-wide lock, and this is what says so.
    //
    // Separate contexts on purpose: a per-context lock would pass a test that shares one, and the state the driver
    // corrupts is the device's — which a Mac hands out once, to everyone who asks.
    //
    // Without the lock this aborts the binary in roughly one run in five, so it is a probabilistic gate: a regression
    // shows up within a few runs of the suite rather than on the first.
    constexpr auto thread_count = 8;
    auto threads = cc::vector<std::thread>::create_with_capacity(thread_count);
    auto built = std::atomic<int>(0);
    auto ready = std::atomic<int>(0);

    for (auto i = 0; i < thread_count; ++i)
        threads.emplace_back(
            [&]
            {
                auto const ctx = mtl::test::make_context();
                if (ctx == nullptr)
                    return;

                // Line the threads up so they reach the compiler together rather than one after another.
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (ready.load(std::memory_order_acquire) < thread_count)
                    std::this_thread::yield();

                // No CHECK on a spawned thread: it would not be attributed to the running test, and what is being
                // proven is the aggregate below — plus the absence of an abort, which no check can express.
                if (make_triangle_pipeline(ctx, sg::pixel_format::rgba8_unorm).has_value())
                    built.fetch_add(1, std::memory_order_acq_rel);
            });

    for (auto& t : threads)
        t.join();

    CHECK(built.load(std::memory_order_acquire) == thread_count);
}

ASYNC_TEST("sg metal - a rendering scope clears and draws")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    constexpr auto k_size = 4;
    auto const target = ctx->persistent.create_texture_2d({
        .format = sg::pixel_format::rgba8_unorm,
        .width = k_size,
        .height = k_size,
        .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src,
    });

    auto pipeline = make_triangle_pipeline(ctx, sg::pixel_format::rgba8_unorm);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    auto cmd = ctx->create_command_list();
    {
        // The triangle covers the whole target, so every texel ends up the fragment colour rather than the clear —
        // which is what makes a wrong result tell the two apart.
        auto info = sg::rendering_info{};
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);
        scope.bind_pipeline(*pipeline.value());
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    auto future = cmd->download.bytes_from_texture(target.raw());
    ctx->submit_command_list(cc::move(cmd));

    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    REQUIRE(bytes.has_value());
    REQUIRE(bytes.value().size() == k_size * k_size * 4);

    // 0.25 / 0.5 / 0.75 in 8-bit unorm, with a texel of slack for the rounding Metal chooses.
    auto const near = [](byte value, int expected)
    {
        auto const delta = int(u8(value)) - expected;
        return (delta < 0 ? -delta : delta) <= 1;
    };

    auto wrong = 0;
    for (auto i = 0; i < k_size * k_size; ++i)
    {
        auto const* const texel = &bytes.value()[i * 4];
        if (!near(texel[0], 64) || !near(texel[1], 128) || !near(texel[2], 191) || !near(texel[3], 255))
            ++wrong;
    }
    CHECK(wrong == 0)
        .context(cc::format("{} of {} texels wrong; first is {} {} {} {}", wrong, k_size * k_size,
                            int(u8(bytes.value()[0])), int(u8(bytes.value()[1])), int(u8(bytes.value()[2])),
                            int(u8(bytes.value()[3]))));
}

ASYNC_TEST("sg metal - an empty rendering scope opens and closes")
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // The rendering scope on its own, with no pipeline and no draw — so a failure here is the encoder setup rather than
    // anything about drawing.
    auto const target = ctx->persistent.create_texture_2d({
        .format = sg::pixel_format::rgba8_unorm,
        .width = 4,
        .height = 4,
        .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src,
    });

    auto cmd = ctx->create_command_list();
    {
        auto info = sg::rendering_info{};
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1)));
        auto scope = cmd->raster.render_to(info);
        (void)scope;
    }
    ctx->submit_command_list(cc::move(cmd));
    co_await ctx->idle_completion();

    CHECK(!ctx->is_device_lost());
}
