#include "shader_fixtures.hh"
#include "texel_formats.hh"

#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/display.hh>
#include <shaped-rendering/exposure_meter_routine.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// sr::exposure_meter_routine: an image's mean log luminance, for an automatic exposure.
//
// The mean is geometric, so a few bright pixels pull an exposure down less than an arithmetic mean would.
// An image half at 0.1 and half at 0.4 therefore meters 0.2 rather than 0.25.

namespace
{
/// Meters a `width` x `height` image whose left half is `left` and right half `right`, in luminance, and returns the luminance it metered.
cc::shared_async<cc::optional<f32>> meter_once(sg::context& ctx, int width, int height, f32 left, f32 right)
{
    sr::exposure_meter_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();

    auto const image
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba32_float,
                                            .width = width,
                                            .height = height,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto texels = cc::vector<tg::vec4f>();
    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
        {
            auto const v = x < width / 2 ? left : right;
            texels.push_back(tg::vec4f(v, v, v, 1));
        }

    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(image.raw(), sr_test::packed(sg::pixel_format::rgba32_float, texels));
    auto const partials = sr::exposure_meter_routine::execute(*cmd, image);
    REQUIRE(partials.has_value()); // prewarmed above, so none here is a compile failure rather than a race
    auto const readback = cmd->download.data_from_buffer(partials.value());
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const& data = co_await readback.data();
    REQUIRE(data.size() == sr::exposure_meter_routine::partial_count);
    co_return sr::exposure_meter_routine::average_luminance(cc::span<tg::vec2f const>(data.data(), data.size()));
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - the exposure meter reads an image's geometric mean luminance",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    // A uniform image meters its own luminance, at any size the grid has to stretch over.
    auto const flat = co_await meter_once(ctx, 64, 64, 0.5f, 0.5f);
    REQUIRE(flat.has_value());
    CHECK(tg::abs(flat.value() - 0.5f) < 1e-4f).dump("metered", flat.value());
    auto const wide = co_await meter_once(ctx, 1000, 30, 0.5f, 0.5f);
    REQUIRE(wide.has_value());
    CHECK(tg::abs(wide.value() - 0.5f) < 1e-4f).dump("metered", wide.value());

    // Half and half meters the geometric mean, which is what middle gray is then placed on.
    auto const halves = co_await meter_once(ctx, 128, 64, 0.1f, 0.4f);
    REQUIRE(halves.has_value());
    CHECK(tg::abs(halves.value() - 0.2f) < 1e-4f).dump("metered", halves.value());
    CHECK(tg::abs(sr::exposure_for_average(halves.value()) - tg::log(0.9f) / tg::log(2.0f)) < 1e-3f);
}

TEST("sr - the exposure meter's mean ignores partials that summed nothing, and has none without samples")
{
    tg::vec2f const none[] = {tg::vec2f(0, 0), tg::vec2f(0, 0)};
    CHECK(!sr::exposure_meter_routine::average_luminance(none).has_value());
    tg::vec2f const some[] = {tg::vec2f(-2.0f * 16, 16), tg::vec2f(0, 0)};
    auto const average = sr::exposure_meter_routine::average_luminance(some);
    REQUIRE(average.has_value());
    CHECK(tg::abs(average.value() - 0.25f) < 1e-6f);
}
