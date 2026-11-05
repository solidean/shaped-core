#include "metal-test-common.hh"
#include "sgl-fixture.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/native_thread.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/binding/compiled_shader.hh>

#include <atomic>

// Raster, end to end: a pipeline built from the SGL package's covering triangle, a rendering scope that clears and draws,
// and the target read back.
//
// The tier-1 suite has no raster execution test, for the same reason it has no compute one — bytecode is per-backend —
// so this tier is the specification for the rendering scope and the draw.

namespace mtl = sg::backend::metal;
using namespace cc::primitive_defines;

namespace
{
namespace draws = mtl::test::shaders::draws;

/// The covering triangle's two stages, compiled once: a library is one per process, and the shaders serve any context.
struct triangle_stages
{
    sg::compiled_shader vertex;
    sg::compiled_shader fragment;
};

[[nodiscard]] cc::shared_async<triangle_stages> compile_triangle(mtl::metal_context& ctx)
{
    co_return triangle_stages{.vertex = co_await mtl::test::sgl_shader(ctx, draws::covering_vs),
                              .fragment = co_await mtl::test::sgl_shader(ctx, draws::covering_ps)};
}

[[nodiscard]] cc::result<sg::backend::metal::metal_raster_pipeline_handle>
make_triangle_pipeline(mtl::metal_context_handle const& ctx, sg::pixel_format format, triangle_stages const& stages)
{
    auto layout = ctx->create_metal_pipeline_layout({}, sg::lifetime_scope::persistent);
    if (layout.has_error())
        return cc::error(layout.error().to_string());

    auto desc = sg::raster_pipeline_description{
        .layout = layout.value(),
        .vertex_shader = stages.vertex,
        .fragment_shader = stages.fragment,
        // No culling: what this fixture exercises is the rendering scope and the draw, and sg's default cull_mode is
        // `back` — so a winding the fixture never meant to assert would otherwise decide whether anything appears.
        .rasterization = {.cull = sg::cull_mode::none},
    };
    desc.color_targets.push_back({.format = format});

    return ctx->create_metal_raster_pipeline(desc, sg::lifetime_scope::persistent);
}
} // namespace

ASYNC_TEST("sg metal - a raster pipeline builds from a metal library", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    auto pipeline = make_triangle_pipeline(ctx, sg::pixel_format::rgba8_unorm, co_await compile_triangle(*ctx));
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    CHECK(pipeline.value()->state() != nullptr);

    // The depth and stencil test is a separate object here, bound on the encoder rather than baked into the pipeline —
    // so it exists even for a pipeline with no depth attachment at all.
    CHECK(pipeline.value()->depth_stencil_state() != nullptr);
}

ASYNC_TEST("sg metal - pipelines build concurrently from several contexts", exclusive("slib-shader-library"))
{
    auto const probe = mtl::test::make_context();
    if (probe == nullptr)
        SKIP("no metal 4 device on this host");

    // Compiled once up front, so what the threads race on is pipeline creation alone.
    auto const stages = co_await compile_triangle(*probe);

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
    auto threads = cc::vector<cc::native_thread>::create_with_capacity(thread_count);
    auto built = std::atomic<int>(0);
    auto ready = std::atomic<int>(0);

    for (auto i = 0; i < thread_count; ++i)
        threads.push_back(cc::native_thread::start(
            [&]
            {
                auto const ctx = mtl::test::make_context();
                if (ctx == nullptr)
                    return;

                // Line the threads up so they reach the compiler together rather than one after another.
                ready.fetch_add(1, std::memory_order_acq_rel);
                while (ready.load(std::memory_order_acquire) < thread_count)
                    cc::this_thread_yield();

                // No CHECK on a spawned thread: it would not be attributed to the running test, and what is being
                // proven is the aggregate below — plus the absence of an abort, which no check can express.
                if (make_triangle_pipeline(ctx, sg::pixel_format::rgba8_unorm, stages).has_value())
                    built.fetch_add(1, std::memory_order_acq_rel);
            }));

    for (auto& t : threads)
        t.join();

    CHECK(built.load(std::memory_order_acquire) == thread_count);
}

ASYNC_TEST("sg metal - a rendering scope clears and draws", exclusive("slib-shader-library"))
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

    auto pipeline = make_triangle_pipeline(ctx, sg::pixel_format::rgba8_unorm, co_await compile_triangle(*ctx));
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

namespace
{
/// A pipeline over the fixture's vertex stage with no fragment stage at all — which is what a depth-only pass is.
[[nodiscard]] cc::result<sg::backend::metal::metal_raster_pipeline_handle> make_depth_only_pipeline(
    mtl::metal_context_handle const& ctx,
    sg::depth_stencil_state const& depth_stencil,
    sg::compiled_shader const& vertex)
{
    auto layout = ctx->create_metal_pipeline_layout({}, sg::lifetime_scope::persistent);
    if (layout.has_error())
        return cc::error(layout.error().to_string());

    return ctx->create_metal_raster_pipeline(
        {
            .layout = layout.value(),
            .vertex_shader = vertex,
            .rasterization = {.cull = sg::cull_mode::none},
            .depth_stencil = depth_stencil,
            .depth_stencil_format = sg::pixel_format::depth32_float,
        },
        sg::lifetime_scope::persistent);
}

/// The depth texels of a 4×4 `depth32_float` target, read back.
[[nodiscard]] cc::shared_async<cc::vector<float>> draw_depth_only(mtl::metal_context_handle ctx,
                                                                  sg::depth_stencil_state depth_stencil)
{
    constexpr auto k_size = 4;
    auto const depth = ctx->persistent.create_texture_2d({
        .format = sg::pixel_format::depth32_float,
        .width = k_size,
        .height = k_size,
        .usage = sg::texture_usage::depth_stencil | sg::texture_usage::copy_src,
    });

    auto const stages = co_await compile_triangle(*ctx);
    auto pipeline = make_depth_only_pipeline(ctx, depth_stencil, stages.vertex);
    CC_ASSERT(pipeline.has_value(), "the depth-only fixture pipeline could not be built");

    auto cmd = ctx->create_command_list();
    {
        auto info = sg::rendering_info{};
        info.depth_stencil_target = depth.as_depth_stencil_view().cleared(1.0f);
        auto scope = cmd->raster.render_to(info);
        scope.bind_pipeline(*pipeline.value());
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    }
    auto future = cmd->download.bytes_from_texture(depth.raw());
    ctx->submit_command_list(cc::move(cmd));
    co_await ctx->idle_completion();

    auto const bytes = future.try_get_bytes();
    CC_ASSERT(bytes.has_value(), "the depth readback never landed");

    auto out = cc::vector<float>();
    auto const* const texels = reinterpret_cast<float const*>(bytes.value().data());
    for (auto i = 0; i < k_size * k_size; ++i)
        out.push_back(texels[i]);
    co_return out;
}
} // namespace

ASYNC_TEST("sg metal - a depth-only pass writes depth", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // A pipeline with no fragment stage is a depth-only pass, and Metal runs one from the vertex stage alone.
    // Turning rasterization off because there is no fragment function discards every primitive before the depth test,
    // so the pass writes nothing and this reads back the clear.
    auto const depth = co_await draw_depth_only(ctx, {.depth_test = true, .depth_write = true});

    REQUIRE(depth.size() == 16);
    auto wrong = 0;
    for (auto const value : depth)
        if (value > 0.001f)
            ++wrong;
    CHECK(wrong == 0).context(cc::format("{} of 16 depth texels were not written; first is {}", wrong, depth[0]));
}

ASYNC_TEST("sg metal - depth is not written with the test off", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // `depth_write` alone is not a licence to write depth through a pass that declared it is not testing it — dx12 and
    // vulkan both write nothing here, and mapping a disabled test to `CompareFunctionAlways` while keeping the write
    // makes metal the odd one out.
    auto const depth = co_await draw_depth_only(ctx, {.depth_test = false, .depth_write = true});

    REQUIRE(depth.size() == 16);
    auto wrong = 0;
    for (auto const value : depth)
        if (value < 0.999f)
            ++wrong;
    CHECK(wrong == 0).context(cc::format("{} of 16 depth texels were written; first is {}", wrong, depth[0]));
}

ASYNC_TEST("sg metal - a stencil-masked draw is masked by the stencil clear", exclusive("slib-shader-library"))
{
    auto const ctx = mtl::test::make_context();
    if (ctx == nullptr)
        SKIP("no metal 4 device on this host");

    // Metal takes a depth attachment and a stencil attachment separately where sg names one target, so a combined
    // format needs both set from the same texture.
    // With only the depth half, the stencil clear never happens and the comparison below reads whatever was in memory:
    // the two draws stop differing, and which of them is wrong is a matter of luck.
    constexpr auto k_size = 4;

    auto const make_color = [&]
    {
        return ctx->persistent.create_texture_2d({
            .format = sg::pixel_format::rgba8_unorm,
            .width = k_size,
            .height = k_size,
            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src,
        });
    };
    auto const masked_out = make_color();
    auto const drawn = make_color();

    auto const depth = ctx->persistent.create_texture_2d({
        .format = sg::pixel_format::depth32_float_stencil8,
        .width = k_size,
        .height = k_size,
        .usage = sg::texture_usage::depth_stencil,
    });

    auto layout = ctx->create_metal_pipeline_layout({}, sg::lifetime_scope::persistent);
    REQUIRE(layout.has_value());

    auto const stages = co_await compile_triangle(*ctx);
    auto desc = sg::raster_pipeline_description{
        .layout = layout.value(),
        .vertex_shader = stages.vertex,
        .fragment_shader = stages.fragment,
        .rasterization = {.cull = sg::cull_mode::none},
        // Draw only where the stencil equals the reference, which each pass sets for itself.
        .depth_stencil = {.stencil_test = true,
                          .stencil_front = {.compare = sg::compare_op::equal},
                          .stencil_back = {.compare = sg::compare_op::equal}},
        .depth_stencil_format = sg::pixel_format::depth32_float_stencil8,
    };
    desc.color_targets.push_back({.format = sg::pixel_format::rgba8_unorm});

    auto pipeline = ctx->create_metal_raster_pipeline(desc, sg::lifetime_scope::persistent);
    REQUIRE(pipeline.has_value()).context(pipeline.has_error() ? pipeline.error().to_string() : cc::string());

    auto cmd = ctx->create_command_list();
    auto const pass = [&](auto const& target, u32 reference)
    {
        auto info = sg::rendering_info{};
        info.color_targets.push_back(target.as_render_target_view().cleared(tg::vec4f(1, 0, 0, 1)));
        info.depth_stencil_target = depth.as_depth_stencil_view().cleared(1.0f, 1);
        auto scope = cmd->raster.render_to(info);
        scope.bind_pipeline(*pipeline.value());
        scope.set_stencil_reference(reference);
        scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    };

    pass(masked_out, 0); // stencil is 1 everywhere, so nothing passes and the clear survives
    pass(drawn, 1);      // matches the clear, so the triangle covers the target

    auto masked_future = cmd->download.bytes_from_texture(masked_out.raw());
    auto drawn_future = cmd->download.bytes_from_texture(drawn.raw());
    ctx->submit_command_list(cc::move(cmd));
    co_await ctx->idle_completion();

    auto const masked_bytes = masked_future.try_get_bytes();
    auto const drawn_bytes = drawn_future.try_get_bytes();
    REQUIRE(masked_bytes.has_value());
    REQUIRE(drawn_bytes.has_value());

    // Red is the clear; the fragment shader writes 0.25/0.5/0.75.
    CHECK(int(u8(masked_bytes.value()[0])) == 255);
    CHECK(int(u8(masked_bytes.value()[1])) == 0);
    CHECK(int(u8(drawn_bytes.value()[0])) < 128)
        .context(cc::format("the stencil-passing draw wrote {}", int(u8(drawn_bytes.value()[0]))));
    CHECK(int(u8(drawn_bytes.value()[1])) > 100);
}
