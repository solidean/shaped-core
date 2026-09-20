#include <clean-core/common/utility.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/mix_routine.hh>
#include <shaped-rendering/shaders.hh>
#include <shaped-shader-library/compiler/dxc_compiler.hh>
#include <shaped-shader-library/shader_library.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;

// sr::mix_routine: one image faded into another, in place.
//
// The endpoints are what a crossfade is judged on: a fade that does not reach its ends shows a step at whichever one
// it misses, and a step is what the fade exists to remove.
// Weight 0 is exact because `lerp` computes `a + (b - a) * w`, so nothing is added to the destination at all.
// Weight 1 is not, since that same form re-derives `b` through a subtraction — which is a rounding error rather than a
// step, and the check says so instead of pretending otherwise.
// The middle is checked per channel, because a blend written against luminance rather than colour passes a grey test
// and tints everything else.

namespace
{
constexpr int k_size = 8;

constexpr auto image_usage = sg::texture_usage::readonly_texture | sg::texture_usage::readwrite_texture
                           | sg::texture_usage::copy_dst | sg::texture_usage::copy_src;

[[nodiscard]] sg::texture_2d make_image(sg::context& ctx)
{
    return ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float, .width = k_size, .height = k_size, .usage = image_usage});
}

[[nodiscard]] cc::vector<tg::vec4f> filled(tg::vec4f v)
{
    return cc::vector<tg::vec4f>::create_filled(k_size * k_size, v);
}

/// Gives `lib` a compiler and sr's package, which the routine needs before it can compile.
/// False when there is no compiler, and the caller skips.
[[nodiscard]] bool add_sr_shaders(slib::shader_library& lib)
{
    auto compiler = slib::create_dxc_compiler();
    if (!compiler.has_value())
        return false;
    lib.add_compiler(cc::move(compiler.value()));
    lib.add_package(sr::shader_package());
    return true;
}

/// Uploads both images, mixes `source` into `destination` by `weight`, and reads the destination back.
cc::shared_async<cc::vector<tg::vec4f>> mix_once(sg::context& ctx, tg::vec4f destination, tg::vec4f source, f32 weight)
{
    sr::mix_routine::prewarm(ctx);
    (void)co_await ctx.routines.idle_completion();

    auto const dst = make_image(ctx);
    auto const src = make_image(ctx);

    auto cmd = ctx.create_command_list();
    auto const dst_pixels = filled(destination);
    auto const src_pixels = filled(source);
    cmd->upload.bytes_to_texture(dst.raw(), cc::span<tg::vec4f const>(dst_pixels).as_bytes());
    cmd->upload.bytes_to_texture(src.raw(), cc::span<tg::vec4f const>(src_pixels).as_bytes());

    auto const ran = sr::mix_routine::execute(*cmd, dst, src, weight);
    REQUIRE(ran); // prewarmed above, so a decline here is a compile failure rather than a race

    auto const readback = sg::data_future<tg::vec4f>(cmd->download.bytes_from_texture(dst.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();

    auto const pixels = co_await readback.data();
    auto out = cc::vector<tg::vec4f>();
    for (auto i = isize(0); i < pixels.size(); ++i)
        out.push_back(pixels[i]);
    co_return out;
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - a mix reaches both of its endpoints exactly",
                     (sg::context_handle const& ctx_h),
                     exclusive("slib-shader-library"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    auto lib = slib::shader_library();
    if (!add_sr_shaders(lib))
        SKIP("no DXC compiler to build the mix shader");

    auto const a = tg::vec4f(0.25f, 0.5f, 0.75f, 1.0f);
    auto const b = tg::vec4f(0.9f, 0.1f, 0.4f, 0.5f);

    // Weight 0 leaves the destination untouched, bit for bit: a fade's first frame is this, and anything else is a
    // visible step into the fade.
    auto const kept = co_await mix_once(ctx, a, b, 0.0f);
    REQUIRE(kept.size() == k_size * k_size);
    for (auto c = 0; c < 4; ++c)
        CHECK(kept[0][c] == a[c]);

    // And weight 1 arrives at the source, to within the one subtraction `lerp` takes to get there.
    auto const replaced = co_await mix_once(ctx, a, b, 1.0f);
    for (auto c = 0; c < 4; ++c)
        CHECK(tg::abs(replaced[0][c] - b[c]) < 1e-6f);
}

ASYNC_INVOCABLE_TEST("sr - a mix interpolates every channel on its own",
                     (sg::context_handle const& ctx_h),
                     exclusive("slib-shader-library"))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;

    auto lib = slib::shader_library();
    if (!add_sr_shaders(lib))
        SKIP("no DXC compiler to build the mix shader");

    // Channels that move in different directions and by different amounts, so a blend that collapsed them onto one
    // scalar — luminance, or the red channel alone — cannot pass.
    auto const a = tg::vec4f(0.0f, 1.0f, 0.25f, 1.0f);
    auto const b = tg::vec4f(1.0f, 0.0f, 0.75f, 0.0f);

    auto const w = 0.25f;
    auto const mixed = co_await mix_once(ctx, a, b, w);
    REQUIRE(mixed.size() == k_size * k_size);

    for (auto c = 0; c < 4; ++c)
    {
        auto const expected = a[c] + (b[c] - a[c]) * w;
        CHECK(tg::abs(mixed[0][c] - expected) < 1e-5f);
    }

    // Every pixel, not just the first: the dispatch is per pixel, and a bounds check written against the wrong extent
    // leaves a band at an edge untouched.
    auto uniform = true;
    for (auto const& p : mixed)
        for (auto c = 0; c < 4; ++c)
            uniform &= tg::abs(p[c] - mixed[0][c]) < 1e-6f;
    CHECK(uniform);
}
