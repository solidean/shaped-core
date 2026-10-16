#include "denoise_reference.hh"
#include "shader_fixtures.hh"
#include "texel_formats.hh"

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <nexus/async-test.hh>
#include <nexus/test.hh>
#include <shaped-graphics/all.hh>
#include <sr_sgl_shaders.hh>
#include <typed-geometry/linalg/vec.hh>
#include <typed-geometry/linalg/vec_ops.hh>
#include <typed-geometry/scalar/scalar.hh>

using namespace cc::primitive_defines;
using namespace sr_test::reference;

// The native denoise members' passes, each run once on the GPU and held to a CPU transcription of what it computes.
// svgf-reference-test.cc holds SVGF's; these are the plain à-trous member's and the depth FSR is handed.
//
// The member tests in denoise-test.cc judge what a filter is for: a flat image stays flat, an edge survives, noise goes down.
// None of them would notice a pass that computes something slightly different, such as a floor written 1e-3 where it was 1e-2, or a weight squared where it was not.
// These do.
// The reference follows the pass statement by statement, in doubles, so it guards against a regression rather than proving the pass right.
// A tolerance covers the GPU's own `exp`, `pow` and `sqrt` and nothing else.
//
// The inputs are deterministic noise with a missed pixel or two in the guides, so every branch of the edge-stops runs.

namespace
{
struct atrous_case
{
    int step = 1;
    u32 flags = 0;
    f64 luminance_scale = 1.0;
    f64 normal_power = 64.0;
    f64 depth_sigma = 0.02;
};

struct atrous_inputs
{
    cc::vector<tg::vec4f> source;
    cc::vector<tg::vec4f> albedo;
    cc::vector<tg::vec4f> specular;
    cc::vector<tg::vec4f> normal;
    cc::vector<tg::vec4f> depth;
    cc::vector<tg::vec4f> counts;
};

[[nodiscard]] atrous_inputs make_atrous_inputs()
{
    auto in = atrous_inputs{.source = field(1, 0.0f, 3.0f),
                            .albedo = field(2, 0.0f, 1.0f),
                            .specular = field(3, 0.0f, 0.5f),
                            .normal = clustered_normals(4),
                            // close enough that the depth edge-stop weighs taps rather than refusing them all
                            .depth = field(5, 0.95f, 1.05f),
                            .counts = field(6, 1.0f, 64.0f)};
    in.normal[5] = tg::vec4f(0, 0, 0, 0); // a missed pixel among the normals
    in.depth[7] = tg::vec4f(-1, 0, 0, 0); // and two among the depths
    in.depth[40] = tg::vec4f(0, 0, 0, 0);
    return in;
}

/// One à-trous pass, as atrous_denoise.sgl computes it.
[[nodiscard]] cc::vector<tg::vec4f> atrous_reference(atrous_case const& k, atrous_inputs const& in)
{
    auto const is = [&](u32 bit) { return (k.flags & bit) != 0; };
    auto const albedo_at = [&](int i)
    {
        auto a = xyz(in.albedo[i]);
        if (is(32u))
            a += xyz(in.specular[i]);
        return vec3(cc::max(a[0], 1e-3), cc::max(a[1], 1e-3), cc::max(a[2], 1e-3));
    };
    auto const source_at = [&](int i)
    {
        auto c = xyz(in.source[i]);
        if (is(8u))
        {
            auto const a = albedo_at(i);
            c = vec3(c[0] / a[0], c[1] / a[1], c[2] / a[2]);
        }
        return c;
    };

    auto out = cc::vector<tg::vec4f>();
    for (auto y = 0; y < k_h; ++y)
        for (auto x = 0; x < k_w; ++x)
        {
            auto const p = y * k_w + x;
            auto const center = source_at(p);
            auto const center_lum = luminance(center);
            auto const n_p = xyz(in.normal[p]);
            auto const center_has_normal = is(2u) && tg::dot(n_p, n_p) > 0.0;
            auto const center_depth = is(4u) ? f64(in.depth[p][0]) : 0.0;
            auto scale = k.luminance_scale;
            if (is(64u))
                scale /= tg::sqrt(cc::max(f64(in.counts[p][0]), 1.0));
            auto const lum_width = scale * (tg::abs(center_lum) + 1e-2) + 1e-6;

            auto sum = vec3(0, 0, 0);
            auto weight_sum = 0.0;
            for (auto dy = -2; dy <= 2; ++dy)
                for (auto dx = -2; dx <= 2; ++dx)
                {
                    auto const qx = x + dx * k.step;
                    auto const qy = y + dy * k.step;
                    if (!inside(qx, qy))
                        continue;
                    auto const q = qy * k_w + qx;
                    auto const c = source_at(q);
                    auto const ratio = (luminance(c) - center_lum) / lum_width;
                    auto w = k_kernel[dx + 2] * k_kernel[dy + 2] * tg::exp(-ratio * ratio);
                    if (is(2u))
                    {
                        auto const n_q = xyz(in.normal[q]);
                        auto const tap_has_normal = tg::dot(n_q, n_q) > 0.0;
                        if (center_has_normal != tap_has_normal)
                            w = 0.0;
                        else if (center_has_normal)
                            w *= tg::pow(cc::clamp(tg::dot(tg::normalize(n_p), tg::normalize(n_q)), 0.0, 1.0),
                                         k.normal_power);
                    }
                    if (is(4u))
                    {
                        auto const d = f64(in.depth[q][0]);
                        if ((center_depth <= 0.0) != (d <= 0.0))
                            w = 0.0;
                        else if (center_depth > 0.0)
                        {
                            auto const distance = tg::sqrt(f64(dx * dx + dy * dy)) * k.step;
                            auto const allowed = k.depth_sigma * center_depth * distance + 1e-5;
                            w *= tg::exp(-tg::abs(d - center_depth) / allowed);
                        }
                    }
                    sum += c * w;
                    weight_sum += w;
                }

            auto result = sum / weight_sum;
            if (is(16u))
                result = times(result, albedo_at(p));
            out.push_back(tg::vec4f(f32(result[0]), f32(result[1]), f32(result[2]), in.source[p][3]));
        }
    return out;
}

/// One pass of case `k` over `in`, written into an image of `format` and read back.
cc::shared_async<cc::vector<tg::vec4f>> run_atrous(sg::context& ctx,
                                                   atrous_case k,
                                                   atrous_inputs const& in,
                                                   sg::pixel_format format)
{
    auto const values = sr::sgl_shaders::atrous_denoise_options{.target_format = format};
    auto const pipeline = co_await settled(sr::sgl_shaders::atrous_denoise.main_cs.acquire_pipeline(ctx, values));
    auto const layout = ctx.cached.acquire_binding_group_layout(
        sr::sgl_shaders::atrous_images::declared_bindings(values), sr::sgl_shaders::atrous_images::declared_samplers());

    auto cmd = ctx.create_command_list();
    auto gpu = gpu_inputs{.ctx = ctx, .cmd = *cmd};
    auto const target = gpu.target(format);
    auto const group = ctx.transient.create_binding_group(
        *cmd, layout,
        sr::sgl_shaders::atrous_images{.source = gpu.texture(in.source).as_texture_view(),
                                       .albedo = gpu.texture(in.albedo).as_texture_view(),
                                       .specular_albedo = gpu.texture(in.specular).as_texture_view(),
                                       .normal = gpu.texture(in.normal).as_texture_view(),
                                       .depth = gpu.texture(in.depth).as_texture_view(),
                                       .sample_counts = gpu.texture(in.counts).as_texture_view(),
                                       .target = target.as_any_image_view()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.set_inline_constants(sr::sgl_shaders::atrous_constants{.step = k.step,
                                                                        .flags = k.flags,
                                                                        .luminance_scale = f32(k.luminance_scale),
                                                                        .normal_power = f32(k.normal_power),
                                                                        .depth_sigma = f32(k.depth_sigma)}
                                          .to_block());
    cmd->compute.dispatch_threads(k_w, k_h, 1);
    ctx.submit_command_list(cc::move(cmd));
    co_return co_await read_back(ctx, target);
}
} // namespace

// One pass of the plain à-trous member with every guide, and with none: the edge-stops, the demodulation round trip and the per-pixel sample count are each in the result or the comparison fails.
ASYNC_INVOCABLE_TEST("sr - an a-trous pass computes what its description says, guide by guide",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const in = make_atrous_inputs();

    // every flag, then no guide at all, then the guides without the albedo round trip
    for (auto const k : {atrous_case{.step = 2, .flags = 0x7fu, .luminance_scale = 0.7},
                         atrous_case{.step = 1, .flags = 0u, .luminance_scale = 1.5},
                         atrous_case{.step = 4, .flags = 0x46u, .luminance_scale = 0.4, .depth_sigma = 0.3}})
    {
        auto const got = co_await run_atrous(ctx, k, in, sg::pixel_format::rgba32_float);
        auto const expected = atrous_reference(k, in);
        CHECK(sr_test::worst_stored(sg::pixel_format::rgba32_float, got, expected, 4) < 2e-5)
            .context(cc::format("flags {:#x}", k.flags));
    }
}

// The last pass writes the caller's image in that image's own format, which the passes before it never do.
ASYNC_INVOCABLE_TEST("sr - an a-trous pass stores what it computes into every output format",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const in = make_atrous_inputs();
    auto const k = atrous_case{.step = 2, .flags = 0x7fu, .luminance_scale = 0.7};
    auto const expected = atrous_reference(k, in);
    for (auto const format : sr_test::k_swept_formats)
    {
        if (!sg::is_portable_image_format(format) && !ctx.supports(sg::feature::extended_image_formats))
            continue;
        auto const got = co_await run_atrous(ctx, k, in, format);
        CHECK(sr_test::worst_stored(format, got, expected, 4) < 2e-5).context(cc::format("pixel_format {}", int(format)));
    }
}

// The device depth FSR is handed: `near / depth` where a ray hit, and the infinitely far plane where it missed.
ASYNC_INVOCABLE_TEST("sr - the depth fsr reads is near over the view depth, and zero where a ray missed",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto depth = field(21, 0.1f, 50.0f);
    depth[3] = tg::vec4f(0, 0, 0, 0);
    depth[4] = tg::vec4f(-2, 0, 0, 0);
    constexpr auto near_plane = 0.25f;

    auto const pipeline = co_await settled(sr::sgl_shaders::fsr_prepare_depth.main_cs.acquire_pipeline(ctx));
    auto const layout = ctx.cached.acquire_binding_group_layout<sr::sgl_shaders::fsr_prepare_depth_io>();
    auto cmd = ctx.create_command_list();
    auto in = gpu_inputs{.ctx = ctx, .cmd = *cmd};
    auto const target = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::r32_float, .width = k_w, .height = k_h, .usage = k_usage});
    auto const group = ctx.transient.create_binding_group(
        *cmd, layout,
        sr::sgl_shaders::fsr_prepare_depth_io{.linear_depth = in.texture(depth).as_texture_view(),
                                              .device_depth = target.as_image_view<sg::pixel_format::r32_float>()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.set_inline_constants(sr::sgl_shaders::fsr_prepare_depth_constants{.near_plane = near_plane}.to_block());
    cmd->compute.dispatch_threads(k_w, k_h, 1);

    auto const readback = sg::data_future<f32>(cmd->download.bytes_from_texture(target.raw()));
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    auto const got = co_await readback.data();
    REQUIRE(got.size() == k_w * k_h);

    // A GPU divides by a reciprocal, a couple of ulps from IEEE's division; a missed ray is zero exactly.
    auto wrong = 0;
    for (auto i = 0; i < k_w * k_h; ++i)
    {
        auto const expected = depth[i][0] > 0.0f ? near_plane / depth[i][0] : 0.0f;
        wrong += tg::abs(got[i] - expected) <= 1e-6f * expected ? 0 : 1;
    }
    CHECK(wrong == 0);
    CHECK(got[3] == 0.0f);
    CHECK(got[4] == 0.0f);
}
