#include "shader_fixtures.hh"
#include "upscale_scene.hh"

#include <clean-core/string/format.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/dlss_rr_routine.hh>
#include <shaped-rendering/reconstruct.hh>

// DLSS Ray Reconstruction's jitter: the sequence the front hands it, and the sign it is handed in.
//
// Built everywhere and run only where DLSS runs — an RTX adapter on dx12 with the SDK fetched — so it SKIPs on every
// other machine, CI included.
// The sign test is metalfx-test's for metalfx_rr: the same stream twice, the second with the jitter flipped, and the
// right sign reconstructs the stripes measurably better.
// Until it has run on an RTX machine, the sign sr hands NGX is a convention rather than a measured one.

namespace
{
using namespace sr_test::upscale_scene;

[[nodiscard]] cc::optional<cc::string_view> why_dlss_rr_cannot_run(sg::context const& ctx)
{
    if (!sr::query_reconstruct_support(ctx).dlss_rr)
        return cc::string_view("no DLSS Ray Reconstruction here — the SDK is fetched on request, and it needs an RTX "
                               "adapter on dx12");
    return {};
}
} // namespace

ASYNC_INVOCABLE_TEST("sr - dlss_rr is handed the halton jitter wherever it upscales", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    sg::context const& ctx = *ctx_h;
    if (auto const why = why_dlss_rr_cannot_run(ctx); why.has_value())
        SKIP(why.value());

    auto const out = tg::vec2i(640, 480);
    auto const settings = sr::reconstruct_settings{.denoiser = sr::denoise_method::dlss_rr,
                                                   .scale = sr::render_scale_preset::performance};
    auto const in = sr::reconstruct_input_extent(ctx, settings, out, sr_test::every_guide());
    REQUIRE(in != out);
    CHECK(sr::reconstruct_jitter(ctx, settings, out, 5, sr_test::every_guide()) == sr::impl::halton_jitter(5, in, out));
    co_return;
}

ASYNC_INVOCABLE_TEST("sr - dlss_rr reconstructs from the jitter the right way round",
                     (sg::context_handle const& ctx_h),
                     exclusive("slib-shader-library"),
                     exclusive("sg-reload-generation"))
{
    REQUIRE(ctx_h != nullptr);
    sg::context& ctx = *ctx_h;
    if (auto const why = why_dlss_rr_cannot_run(ctx); why.has_value())
        SKIP(why.value());

    (void)sr_test::shader_fixtures();
    sr::dlss_rr_routine::prewarm(ctx);
    sr::reconstruct_routine::prewarm(ctx, sg::pixel_format::rgba32_float);
    (void)co_await ctx.routines.idle_completion();

    auto const honest = co_await run_denoised(ctx, sr::denoise_method::dlss_rr, 32, 0.0f, k_out);
    auto const flipped = co_await run_denoised(ctx, sr::denoise_method::dlss_rr, 32, 0.0f, k_out, -1.0f);
    REQUIRE(honest.last.status == sr::reconstruct_status::denoised);
    REQUIRE(flipped.last.status == sr::reconstruct_status::denoised);

    auto const error = rmse_against_scene(honest.output, 0.0f);
    auto const flipped_error = rmse_against_scene(flipped.output, 0.0f);
    CHECK(error < 0.8f * flipped_error).context(cc::format("dlss_rr {}, flipped jitter {}", error, flipped_error));
    co_await ctx.idle_completion();
}
