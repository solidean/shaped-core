#include "../pipeline/pipeline_harness.hh"
#include "../shaders/shader_fixtures.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <sg_test_mixed_dxil.hh>
#include <sg_test_mixed_spirv.hh>
#include <sg_test_mixed_text.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-library/binding/binding_groups.hh>

using namespace cc::primitive_defines;

// One shader through DXC and through the context's native route: sg reads both one way, so every value logs the same.
// A failed assert is what a test can see of a record, and an exact count makes either route's line count for both.
// A print's record goes through the same decoder, which logs a warning for an id or a length it cannot read.

namespace
{
constexpr cc::string_view k_compute_line
    = "diagnostic_routes.sgl:14: assert failed: int 3, uint 3, float 3.0, lanes int2(3, 7), "
      "near float3(0.5, 2.0, 3.0), bool true, tone 1";
constexpr cc::string_view k_pixel_line = "diagnostic_routes.sgl:32: assert failed: pixel (1, 2) of the target";

/// The window is asked for once the epoch retires, decoded once its bytes are back, and reset before the test ends.
cc::shared_async<cc::unit> drained(sg::context& ctx)
{
    ctx.advance_epoch();
    co_await ctx.idle_completion();
    co_await ctx.idle_completion();
    ctx.process_completed_epochs();
    co_await ctx.idle_completion();
}

template <class Group, class Shader>
cc::shared_async<cc::vector<int>> dispatched(sg::context& ctx, Shader const& shader)
{
    auto const pipeline = co_await shader.acquire_pipeline(ctx);
    auto const layout = ctx.cached.acquire_binding_group_layout<Group>();
    auto const values = ctx.persistent.create_buffer_from_data(
        cc::vector<int>{1, 2, 3, 4}, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src);

    auto cmd = ctx.create_command_list();
    auto const group = ctx.transient.create_binding_group(*cmd, layout, Group{.values = values.as_readwrite_buffer()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.dispatch_threads(4);
    auto const back = cmd->download.data_from_buffer(values);
    ctx.submit_command_list(cc::move(cmd));

    co_await drained(ctx);
    auto const got = co_await back.data();
    co_return cc::vector<int>::create_copy_of(cc::span<int const>(got));
}

template <class Pipeline>
cc::shared_async<sg_test::offscreen_pixels> drawn(sg::context& ctx, Pipeline const& description, cc::string_view target_set)
{
    auto const pipeline = co_await ctx.cached.acquire_raster_pipeline(description);
    auto pixels = co_await sg_test::draw_offscreen(
        ctx, {.width = 4, .height = 4, .colors = {sg::pixel_format::rgba8_unorm}, .target_set = target_set},
        [&](sg::rendering_scope& scope)
        {
            scope.bind_pipeline(*pipeline);
            scope.draw({.vertex_range = {.offset = 0, .size = 3}});
        });
    co_await drained(ctx);
    co_return pixels;
}
} // namespace

ASYNC_INVOCABLE_TEST("sg - a compute stage's failed assert reads the same through DXC and through the native route",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    auto const takes_dxil = ctx->accepts_shader_format(sg::shader_format::dxil);
    auto const takes_spirv = ctx->accepts_shader_format(sg::shader_format::spirv);
    if (!takes_dxil && !takes_spirv)
        SKIP("this context takes no native route");
    // TEMPORARY(bug-sg-amd-dx12-assert-branch-clobbers-value): AMD's dx12 driver stores the failing lane's value after it.
    if (takes_dxil && ctx->metrics.adapter().vendor_id == 0x1002)
        SKIP("AMD's dx12 driver stores the failing lane's value after a failed assert; "
             "issue: bug-sg-amd-dx12-assert-branch-clobbers-value");
    nx::expect_error(k_compute_line, nx::exactly(2));

    namespace text = sg::test::mixed_text::diagnostic_routes;
    auto const through_dxc = co_await dispatched<text::checked>(*ctx, text::stamped);
    auto through_native = cc::vector<int>();
    if (takes_dxil)
    {
        namespace native = sg::test::mixed_dxil::diagnostic_routes;
        auto const shader = co_await native::stamped->acquire(*ctx);
        CHECK(shader.compiler.name == "sgl");
        CHECK(shader.format == sg::shader_format::dxil);
        through_native = co_await dispatched<native::checked>(*ctx, native::stamped);
    }
    else
    {
        namespace native = sg::test::mixed_spirv::diagnostic_routes;
        auto const shader = co_await native::stamped->acquire(*ctx);
        CHECK(shader.compiler.name == "sgl");
        CHECK(shader.format == sg::shader_format::spirv);
        through_native = co_await dispatched<native::checked>(*ctx, native::stamped);
    }

    // the failing invocation returns ahead of its store
    REQUIRE(through_dxc.size() == 4);
    REQUIRE(through_native.size() == 4);
    for (auto i = isize(0); i < 4; ++i)
        CHECK(through_native[i] == through_dxc[i]);
    CHECK(through_dxc[0] == 2);
    CHECK(through_dxc[2] == 3);
}

ASYNC_INVOCABLE_TEST("sg - a pixel stage's failed assert reads the same through DXC and through the native route",
                     (sg::context_handle const& ctx))
{
    REQUIRE(ctx != nullptr);
    if (!sg_test::shaders_reach(*ctx))
        SKIP("no compiler builds this binary's shaders into a format this context accepts");
    auto const takes_dxil = ctx->accepts_shader_format(sg::shader_format::dxil);
    auto const takes_spirv = ctx->accepts_shader_format(sg::shader_format::spirv);
    if (!takes_dxil && !takes_spirv)
        SKIP("this context takes no native route");
    nx::expect_error(k_pixel_line, nx::exactly(2));

    namespace text = sg::test::mixed_text::diagnostic_routes;
    auto const through_dxc = co_await drawn(*ctx, text::marked_pixels, text::marked_target::name);
    auto through_native = sg_test::offscreen_pixels();
    if (takes_dxil)
    {
        namespace native = sg::test::mixed_dxil::diagnostic_routes;
        CHECK((co_await native::marked_ps->acquire(*ctx)).compiler.name == "sgl");
        through_native = co_await drawn(*ctx, native::marked_pixels, native::marked_target::name);
    }
    else
    {
        namespace native = sg::test::mixed_spirv::diagnostic_routes;
        CHECK((co_await native::marked_ps->acquire(*ctx)).compiler.name == "sgl");
        through_native = co_await drawn(*ctx, native::marked_pixels, native::marked_target::name);
    }

    // the failing pixel is discarded, and every other one is drawn
    for (auto y = 0; y < 4; ++y)
        for (auto x = 0; x < 4; ++x)
        {
            auto const a = through_dxc.colors[0].at<u32>(x, y);
            auto const b = through_native.colors[0].at<u32>(x, y);
            CHECK(a == b).context(cc::format("pixel ({}, {})", x, y));
            CHECK(a == (x == 1 && y == 2 ? 0u : 0xFF0000FFu)).context(cc::format("pixel ({}, {})", x, y));
        }
}
