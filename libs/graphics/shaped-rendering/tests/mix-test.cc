#include "shader_fixtures.hh"
#include "texel_formats.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/mix_routine.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// sr::mix_routine: one image faded into another, in place.
//
// The endpoints are what a crossfade is judged on: a fade that does not reach its ends shows a step at whichever one
// it misses, and a step is what the fade exists to remove.
// Weight 0 is exact because `mix` computes `a + (b - a) * w`, so nothing is added to the destination at all.
// Weight 1 is not, since that same form re-derives `b` through a subtraction — which is a rounding error rather than a
// step, and the check says so instead of pretending otherwise.
// The middle is checked per channel, because a blend written against luminance rather than colour passes a grey test
// and tints everything else.
// Each is checked in every format sr_test sweeps, since the routine writes the destination in that destination's own.

namespace
{
constexpr int k_size = 8;

constexpr auto image_usage
    = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_dst | sg::texture_usage::copy_src;

[[nodiscard]] sg::texture_2d make_image(sg::context& ctx, sg::pixel_format format)
{
    return ctx.persistent.create_texture_2d({.format = format, .width = k_size, .height = k_size, .usage = image_usage});
}

/// Whether `ctx` can hand the routine a destination of `format` at all.
[[nodiscard]] bool can_write(sg::context const& ctx, sg::pixel_format format)
{
    return sg::is_portable_image_format(format) || ctx.supports(sg::feature::extended_image_formats);
}

/// Uploads both images, mixes `source` into a `format` destination by `weight`, and reads the destination back.
/// The source stays rgba32_float, since the routine samples it whatever the destination's format.
cc::shared_async<cc::vector<tg::vec4f>> mix_once(sg::context& ctx,
                                                 sg::pixel_format format,
                                                 tg::vec4f destination,
                                                 tg::vec4f source,
                                                 f32 weight)
{
    sr::mix_routine::prewarm(ctx, format);
    (void)co_await ctx.routines.idle_completion();

    auto const dst = make_image(ctx, format);
    auto const src = make_image(ctx, sg::pixel_format::rgba32_float);

    auto cmd = ctx.create_command_list();
    auto const dst_bytes = sr_test::packed(format, cc::vector<tg::vec4f>::create_filled(k_size * k_size, destination));
    auto const src_bytes
        = sr_test::packed(sg::pixel_format::rgba32_float, cc::vector<tg::vec4f>::create_filled(k_size * k_size, source));
    cmd->upload.bytes_to_texture(dst.raw(), dst_bytes);
    cmd->upload.bytes_to_texture(src.raw(), src_bytes);

    auto const ran = sr::mix_routine::execute(*cmd, dst, src, weight);
    REQUIRE(ran); // prewarmed above, so a decline here is a compile failure rather than a race

    auto const readback = cmd->download.bytes_from_texture(dst.raw());
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const& bytes = co_await readback.bytes();
    co_return sr_test::unpacked(format, bytes, k_size * k_size);
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - a mix reaches both of its endpoints exactly",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const a = tg::vec4f(0.25f, 0.5f, 0.75f, 1.0f);
    auto const b = tg::vec4f(0.9f, 0.1f, 0.4f, 0.5f);

    for (auto const format : sr_test::k_swept_formats)
    {
        if (!can_write(ctx, format))
            continue;
        auto const what = cc::format("pixel_format {}", int(format));

        // Weight 0 leaves the destination untouched, bit for bit: a fade's first frame is this, and anything else is a
        // visible step into the fade.
        auto const kept = co_await mix_once(ctx, format, a, b, 0.0f);
        REQUIRE(kept.size() == k_size * k_size);
        auto const stored = sr_test::quantized(format, a);
        for (auto c = 0; c < 4; ++c)
            CHECK(kept[0][c] == stored[c]).context(what);

        // And weight 1 arrives at the source, to within the one subtraction `lerp` takes to get there and the store's
        // own rounding.
        auto const replaced = co_await mix_once(ctx, format, a, b, 1.0f);
        tg::vec4f const wanted[] = {b};
        CHECK(sr_test::worst_stored(format, cc::span<tg::vec4f const>(replaced).first_n(1), wanted, 4) < 1e-6).context(what);
    }
}

ASYNC_INVOCABLE_TEST("sr - a mix interpolates every channel on its own",
                     (sg::context_handle const& ctx_h),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    // Channels that move in different directions and by different amounts, so a blend that collapsed them onto one
    // scalar — luminance, or the red channel alone — cannot pass.
    auto const a = tg::vec4f(0.0f, 1.0f, 0.25f, 1.0f);
    auto const b = tg::vec4f(1.0f, 0.0f, 0.75f, 0.0f);
    auto const w = 0.25f;

    for (auto const format : sr_test::k_swept_formats)
    {
        if (!can_write(ctx, format))
            continue;
        auto const what = cc::format("pixel_format {}", int(format));

        auto const mixed = co_await mix_once(ctx, format, a, b, w);
        REQUIRE(mixed.size() == k_size * k_size);

        // The destination holds what was stored of `a`, and the blend of it is stored again.
        auto const held = sr_test::quantized(format, a);
        tg::vec4f const expected[] = {held + (b - held) * w};
        CHECK(sr_test::worst_stored(format, cc::span<tg::vec4f const>(mixed).first_n(1), expected, 4) < 1e-5).context(what);

        // Every pixel, not just the first: the dispatch is per pixel, and a bounds check written against the wrong
        // extent leaves a band at an edge untouched.
        auto uniform = true;
        for (auto const& p : mixed)
            for (auto c = 0; c < 4; ++c)
                uniform &= tg::abs(p[c] - mixed[0][c]) < 1e-6f;
        CHECK(uniform).context(what);
    }
}
