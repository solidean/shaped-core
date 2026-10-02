#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/blit_routine.hh>

using namespace cc::primitive_defines;

// sr::blit_routine: a source texture drawn across the target of an open rendering scope.
//
// At the same size every target pixel centre is a source texel centre, so a linear filter reproduces each texel.
// That makes the copy exact enough to catch a flipped axis, a swapped channel or a triangle that misses a corner.

namespace
{
constexpr auto extent = 4;

/// Distinct in every channel of every texel, so any texel read from the wrong place is a wrong number.
[[nodiscard]] u8 channel_value(int x, int y, int c)
{
    return u8(16 * (y * extent + x) + 4 * c + 1);
}

[[nodiscard]] cc::vector<byte> source_texels()
{
    auto out = cc::vector<byte>();
    for (auto y = 0; y < extent; ++y)
        for (auto x = 0; x < extent; ++x)
            for (auto c = 0; c < 4; ++c)
                out.push_back(byte(channel_value(x, y, c)));
    return out;
}

/// Blits the source into a fresh target of `format` and reads the target back.
[[nodiscard]] cc::shared_async<cc::pinned_data<byte const>> blit_into(sg::context& ctx, sg::pixel_format format)
{
    // a helper is an unhomed async: it moves to where the device lives before its first bound call
    if (auto* const home = ctx.device_home())
        co_await cc::async_resume_on(*home);

    auto const source
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba8_unorm,
                                            .width = extent,
                                            .height = extent,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    auto const target
        = ctx.persistent.create_texture_2d({.format = format,
                                            .width = extent,
                                            .height = extent,
                                            .usage = sg::texture_usage::render_target | sg::texture_usage::copy_src});

    // WORKAROUND, and here to be found again: a tick drives only routines that are already REGISTERED, and `execute`
    // is what registers one — so a caller meeting a format for the first time declines that frame.
    // It goes away once a routine's readiness is an async; see libs/graphics/shaped-graphics/docs/TODO.md, "Readiness as an async".
    sr::blit_routine::prewarm(ctx, format);
    (void)co_await ctx.routines.idle_completion();

    // The source is uploaded in this list and first sampled inside the scope, so its barrier is found only at the draw,
    // and a backend that cannot hold it inside a pass splits the scope for it.
    // Nothing states a texture's first use before a scope yet; libs/graphics/shaped-graphics/docs/TODO.md, "Barriers + access tracking".
    nx::allow_warnings("was closed and reopened around a barrier", "sg");

    auto cmd = ctx.create_command_list();
    cmd->upload.bytes_to_texture(source.raw(), source_texels());
    {
        auto pass = cmd->raster.render_to({.color_targets = {target.as_render_target_view().discarded()}});
        CHECK(sr::blit_routine::execute(pass, source) == sg::routine_outcome::executed);
    }
    auto const future = cmd->download.bytes_from_texture(target.raw());
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const bytes = co_await future.bytes();
    co_return bytes;
}

/// Every target texel against the source texel at the same place, reading the target's channels in `order`.
void check_copy(cc::span<byte const> pixels, cc::span<int const> order)
{
    REQUIRE(pixels.size() == isize(extent) * extent * 4);
    for (auto y = 0; y < extent; ++y)
        for (auto x = 0; x < extent; ++x)
            for (auto c = 0; c < 4; ++c)
            {
                auto const got = int(u8(pixels[(isize(y) * extent + x) * 4 + order[c]]));
                auto const expected = int(channel_value(x, y, c));

                // One step either way: a filter weight of exactly one is what the arithmetic says, and a GPU is allowed
                // to round it; a texel from the wrong place is at least four steps away.
                auto const within_a_step = got >= expected - 1 && got <= expected + 1;
                CHECK(within_a_step)
                    .context(cc::format("texel {},{} channel {}: got {}, expected {}", x, y, c, got, expected));
            }
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - blit copies a texture texel for texel at the same size",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    auto const pixels = co_await blit_into(*ctx_h, sg::pixel_format::rgba8_unorm);
    int const rgba[] = {0, 1, 2, 3};
    check_copy(pixels, rgba);
}

// The routine keeps one pipeline per target format, and a swapchain is typically bgra8.
// Reading the target's bytes as b, g, r, a and still finding the source's r, g, b, a is what shows the channels land by name.
ASYNC_INVOCABLE_TEST("sr - blit into a bgra8 target keeps each channel",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    auto const pixels = co_await blit_into(*ctx_h, sg::pixel_format::bgra8_unorm);
    int const bgra[] = {2, 1, 0, 3};
    check_copy(pixels, bgra);
}
