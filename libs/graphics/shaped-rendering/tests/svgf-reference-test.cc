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

// SVGF's three passes, each run on the GPU from inputs of its own and held to a CPU transcription of what it computes.
// One wrong pass cannot hide behind another, and denoise-reference-test.cc says what such a reference does and does not prove.

namespace
{
// The flag bits svgf.sgl tests.
constexpr u32 k_has_albedo = 0x1u;
constexpr u32 k_reset = 0x2u;
constexpr u32 k_remodulate_out = 0x4u;
constexpr u32 k_has_specular_albedo = 0x8u;

struct svgf_inputs
{
    cc::vector<tg::vec4f> color;
    cc::vector<tg::vec4f> albedo;
    cc::vector<tg::vec4f> specular;
    cc::vector<tg::vec4f> normal;
    cc::vector<tg::vec4f> depth;
    cc::vector<tg::vec4f> motion;
    /// xyz demodulated color, w a history length on the temporal pass's side and a variance on the filter's
    cc::vector<tg::vec4f> history;
    cc::vector<tg::vec4f> moments;
    cc::vector<tg::vec4f> normal_depth;
};

[[nodiscard]] svgf_inputs make_svgf_inputs()
{
    auto in = svgf_inputs{.color = field(11, 0.0f, 3.0f),
                          .albedo = field(12, 0.0f, 1.0f),
                          .specular = field(13, 0.0f, 0.5f),
                          .normal = field(14, -1.0f, 1.0f),
                          .depth = field(15, 0.5f, 1.5f),
                          .motion = field(16, -1.5f, 1.5f),
                          .history = field(17, 0.0f, 2.0f),
                          .moments = field(18, 0.0f, 1.0f),
                          .normal_depth = clustered_normals(19)};
    in.normal[3] = tg::vec4f(0, 0, 0, 0);
    in.depth[9] = tg::vec4f(0, 0, 0, 0);
    for (auto i = 0; i < k_w * k_h; ++i)
    {
        in.history[i][3] = f32(1 + i % 7); // a history length in alpha, young and old pixels mixed
        in.normal_depth[i][3] = 0.6f + 0.3f * noise(i, 99);
    }
    in.normal_depth[11] = tg::vec4f(0, 0, 0, 0);
    return in;
}

[[nodiscard]] f64 reflectance_component(f64 albedo, f64 specular, u32 flags)
{
    return cc::max((flags & k_has_specular_albedo) != 0 ? albedo + specular : albedo, 1e-3);
}

[[nodiscard]] bool is_surface(vec3 n, f64 depth)
{
    return tg::dot(n, n) > 0.0 && depth > 0.0;
}

[[nodiscard]] f64 geometry_weight(tg::vec4f nd_p, tg::vec4f nd_q, f64 offset_length, f64 normal_power, f64 depth_sigma)
{
    auto const surface_p = is_surface(xyz(nd_p), nd_p[3]);
    auto const surface_q = is_surface(xyz(nd_q), nd_q[3]);
    if (surface_p != surface_q)
        return 0.0;
    if (!surface_p)
        return 1.0;
    auto const w_n
        = tg::pow(cc::clamp(tg::dot(tg::normalize(xyz(nd_p)), tg::normalize(xyz(nd_q))), 0.0, 1.0), normal_power);
    auto const allowed = depth_sigma * f64(nd_p[3]) * cc::max(offset_length, 1.0) + 1e-5;
    return w_n * tg::exp(-tg::abs(f64(nd_q[3]) - f64(nd_p[3])) / allowed);
}

// ---- temporal -------------------------------------------------------------------------------------------------------

constexpr auto k_color_alpha_min = 0.05;
constexpr auto k_moments_alpha_min = 0.2;
constexpr auto k_max_history = 4.0;
constexpr auto k_normal_similarity = 0.2;
constexpr auto k_depth_similarity = 0.5;

struct temporal_result
{
    cc::vector<tg::vec4f> history;
    cc::vector<tg::vec4f> moments;
    cc::vector<tg::vec4f> normal_depth;
    /// how many pixels took their history
    int taken = 0;
};

/// The temporal pass, as svgf.sgl's `temporal_cs` computes it.
[[nodiscard]] temporal_result temporal_reference(svgf_inputs const& in, u32 flags)
{
    auto out = temporal_result{};
    for (auto y = 0; y < k_h; ++y)
        for (auto x = 0; x < k_w; ++x)
        {
            auto const p = y * k_w + x;
            auto c = xyz(in.color[p]);
            if ((flags & k_has_albedo) != 0)
                for (auto i = 0; i < 3; ++i)
                    c[i] /= reflectance_component(in.albedo[p][i], in.specular[p][i], flags);
            auto const n = xyz(in.normal[p]);
            auto const d = f64(in.depth[p][0]);
            auto const lum = luminance(c);
            auto const qx = int(tg::floor(f64(f32(f32(x) + 0.5f - in.motion[p][0]))));
            auto const qy = int(tg::floor(f64(f32(f32(y) + 0.5f - in.motion[p][1]))));
            auto valid = (flags & k_reset) == 0 && inside(qx, qy);
            auto const q = valid ? qy * k_w + qx : 0;
            if (valid)
            {
                auto const prev = in.normal_depth[q];
                auto const surface = is_surface(n, d);
                if (surface != is_surface(xyz(prev), prev[3]))
                    valid = false;
                else if (surface)
                    valid = tg::dot(tg::normalize(n), tg::normalize(xyz(prev))) >= k_normal_similarity
                         && tg::abs(f64(prev[3]) - d) / cc::max(d, 1e-4) <= k_depth_similarity;
            }
            auto out_color = c;
            auto out_m0 = lum;
            auto out_m1 = lum * lum;
            auto length = 1.0;
            if (valid)
            {
                ++out.taken;
                length = cc::min(f64(in.history[q][3]) + 1.0, k_max_history);
                auto const ca = cc::max(1.0 / length, k_color_alpha_min);
                auto const ma = cc::max(1.0 / length, k_moments_alpha_min);
                out_color = xyz(in.history[q]) + (c - xyz(in.history[q])) * ca;
                out_m0 = in.moments[q][0] + (lum - in.moments[q][0]) * ma;
                out_m1 = in.moments[q][1] + (lum * lum - in.moments[q][1]) * ma;
            }
            out.history.push_back(tg::vec4f(f32(out_color[0]), f32(out_color[1]), f32(out_color[2]), f32(length)));
            out.moments.push_back(tg::vec4f(f32(out_m0), f32(out_m1), 0, 0));
            out.normal_depth.push_back(tg::vec4f(in.normal[p][0], in.normal[p][1], in.normal[p][2], in.depth[p][0]));
        }
    return out;
}

/// The temporal pass under `flags`, run on the GPU, its three outputs read back.
cc::shared_async<temporal_result> run_temporal(sg::context& ctx, svgf_inputs const& in, u32 flags)
{
    auto const pipeline = co_await settled(sr::sgl_shaders::svgf.temporal_cs.acquire_pipeline(ctx));
    auto const layout = ctx.cached.acquire_binding_group_layout<sr::sgl_shaders::svgf_temporal_images>();
    auto cmd = ctx.create_command_list();
    auto gpu = gpu_inputs{.ctx = ctx, .cmd = *cmd};
    auto const out_history = gpu.target();
    auto const out_moments = gpu.target(sg::pixel_format::rg32_float);
    auto const out_normal_depth = gpu.target();
    auto const group = ctx.transient.create_binding_group(
        *cmd, layout,
        sr::sgl_shaders::svgf_temporal_images{
            .color = gpu.texture(in.color).as_texture_view(),
            .albedo = gpu.texture(in.albedo).as_texture_view(),
            .specular_albedo = gpu.texture(in.specular).as_texture_view(),
            .normal = gpu.texture(in.normal).as_texture_view(),
            .depth = gpu.texture(in.depth).as_texture_view(),
            .motion = gpu.texture(in.motion).as_texture_view(),
            .previous_history = gpu.texture(in.history).as_texture_view(),
            .previous_moments = gpu.texture(in.moments, sg::pixel_format::rg32_float).as_texture_view(),
            .previous_normal_depth = gpu.texture(in.normal_depth).as_texture_view(),
            .history = out_history.as_image_view<sg::pixel_format::rgba32_float>(),
            .moments = out_moments.as_image_view<sg::pixel_format::rg32_float>(),
            .normal_depth = out_normal_depth.as_image_view<sg::pixel_format::rgba32_float>()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.set_inline_constants(sr::sgl_shaders::svgf_temporal_constants{.flags = flags,
                                                                               .color_alpha_min = k_color_alpha_min,
                                                                               .moments_alpha_min = k_moments_alpha_min,
                                                                               .max_history = k_max_history,
                                                                               .normal_similarity = k_normal_similarity,
                                                                               .depth_similarity = k_depth_similarity}
                                          .to_block());
    cmd->compute.dispatch_threads(k_w, k_h, 1);
    ctx.submit_command_list(cc::move(cmd));

    co_return temporal_result{.history = co_await read_back(ctx, out_history),
                              .moments = co_await read_back(ctx, out_moments),
                              .normal_depth = co_await read_back(ctx, out_normal_depth)};
}

// ---- filter ---------------------------------------------------------------------------------------------------------

constexpr auto k_step = 2;
constexpr auto k_luminance_sigma = 4.0;
constexpr auto k_normal_power = 64.0;
constexpr auto k_depth_sigma = 0.5;

/// One filter pass, as svgf.sgl's `atrous_cs` computes it.
[[nodiscard]] cc::vector<tg::vec4f> filter_reference(svgf_inputs const& in, u32 flags)
{
    auto out = cc::vector<tg::vec4f>();
    for (auto y = 0; y < k_h; ++y)
        for (auto x = 0; x < k_w; ++x)
        {
            auto const p = y * k_w + x;
            auto blurred = 0.0;
            auto blur_weight = 0.0;
            for (auto by = -1; by <= 1; ++by)
                for (auto bx = -1; bx <= 1; ++bx)
                {
                    auto const qx = cc::clamp(x + bx, 0, k_w - 1);
                    auto const qy = cc::clamp(y + by, 0, k_h - 1);
                    auto const w = (bx == 0 ? 0.5 : 0.25) * (by == 0 ? 0.5 : 0.25);
                    blurred += in.history[qy * k_w + qx][3] * w;
                    blur_weight += w;
                }
            blurred /= blur_weight;
            auto const lum_width = k_luminance_sigma * tg::sqrt(cc::max(blurred, 0.0)) + 1e-6;
            auto const center_lum = luminance(xyz(in.history[p]));

            auto sum = vec3(0, 0, 0);
            auto variance_sum = 0.0;
            auto weight_sum = 0.0;
            for (auto dy = -2; dy <= 2; ++dy)
                for (auto dx = -2; dx <= 2; ++dx)
                {
                    auto const qx = x + dx * k_step;
                    auto const qy = y + dy * k_step;
                    if (!inside(qx, qy))
                        continue;
                    auto const q = qy * k_w + qx;
                    auto const length = tg::sqrt(f64(dx * dx + dy * dy)) * k_step;
                    auto w
                        = k_kernel[dx + 2] * k_kernel[dy + 2]
                        * geometry_weight(in.normal_depth[p], in.normal_depth[q], length, k_normal_power, k_depth_sigma);
                    w *= tg::exp(-tg::abs(luminance(xyz(in.history[q])) - center_lum) / lum_width);
                    sum += xyz(in.history[q]) * w;
                    variance_sum += in.history[q][3] * w * w;
                    weight_sum += w;
                }
            auto result = sum / weight_sum;
            auto alpha = variance_sum / (weight_sum * weight_sum);
            if ((flags & k_remodulate_out) != 0)
            {
                if ((flags & k_has_albedo) != 0)
                    for (auto i = 0; i < 3; ++i)
                        result[i] *= reflectance_component(in.albedo[p][i], in.specular[p][i], flags);
                alpha = in.color[p][3];
            }
            out.push_back(tg::vec4f(f32(result[0]), f32(result[1]), f32(result[2]), f32(alpha)));
        }
    return out;
}

/// One filter pass under `flags`, written into an image of `format` and read back.
cc::shared_async<cc::vector<tg::vec4f>> run_filter(sg::context& ctx, svgf_inputs const& in, u32 flags, sg::pixel_format format)
{
    auto const values = sr::sgl_shaders::svgf_options{.target_format = format};
    auto const pipeline = co_await settled(sr::sgl_shaders::svgf.atrous_cs.acquire_pipeline(ctx, values));
    auto const layout
        = ctx.cached.acquire_binding_group_layout(sr::sgl_shaders::svgf_atrous_images::declared_bindings(values),
                                                  sr::sgl_shaders::svgf_atrous_images::declared_samplers());
    auto cmd = ctx.create_command_list();
    auto gpu = gpu_inputs{.ctx = ctx, .cmd = *cmd};
    auto const target = gpu.target(format);
    auto const group = ctx.transient.create_binding_group(
        *cmd, layout,
        sr::sgl_shaders::svgf_atrous_images{.source = gpu.texture(in.history).as_texture_view(),
                                            .normal_depth = gpu.texture(in.normal_depth).as_texture_view(),
                                            .albedo = gpu.texture(in.albedo).as_texture_view(),
                                            .specular_albedo = gpu.texture(in.specular).as_texture_view(),
                                            .color = gpu.texture(in.color).as_texture_view(),
                                            .target = target.as_any_image_view()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.set_inline_constants(sr::sgl_shaders::svgf_atrous_constants{.step = k_step,
                                                                             .flags = flags,
                                                                             .luminance_sigma = k_luminance_sigma,
                                                                             .normal_power = k_normal_power,
                                                                             .depth_sigma = k_depth_sigma}
                                          .to_block());
    cmd->compute.dispatch_threads(k_w, k_h, 1);
    ctx.submit_command_list(cc::move(cmd));
    co_return co_await read_back(ctx, target);
}
} // namespace

// The history taken where the surface matches, with and without the albedo round trip, and refused everywhere on a reset.
ASYNC_INVOCABLE_TEST("sr - svgf's temporal pass computes what its description says", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const in = make_svgf_inputs();
    constexpr auto rgba32 = sg::pixel_format::rgba32_float;
    for (auto const flags :
         {k_has_albedo | k_has_specular_albedo, u32(0), k_reset | k_has_albedo | k_has_specular_albedo})
    {
        auto const what = cc::format("flags {:#x}", flags);
        auto const got = co_await run_temporal(ctx, in, flags);
        auto const expected = temporal_reference(in, flags);

        // A reset takes no history at all; otherwise both kinds of pixel are present, or the comparison says nothing about one of them.
        if ((flags & k_reset) != 0)
            CHECK(expected.taken == 0).context(what);
        else
        {
            CHECK(expected.taken > 0).context(what);
            CHECK(expected.taken < k_w * k_h).context(what);
        }

        CHECK(sr_test::worst_stored(rgba32, got.history, expected.history, 4) < 2e-5).context(what + ", history");
        CHECK(sr_test::worst_stored(rgba32, got.moments, expected.moments, 2) < 2e-5).context(what + ", moments");
        CHECK(sr_test::worst_stored(rgba32, got.normal_depth, expected.normal_depth, 4) == 0.0)
            .context(what + ", normal_depth");
    }
}

// Young pixels estimated spatially and old ones read off their moments.
ASYNC_INVOCABLE_TEST("sr - svgf's variance pass computes what its description says", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const in = make_svgf_inputs();
    constexpr auto spatial_below = 4.0;

    auto const pipeline = co_await settled(sr::sgl_shaders::svgf.variance_cs.acquire_pipeline(ctx));
    auto const layout = ctx.cached.acquire_binding_group_layout<sr::sgl_shaders::svgf_variance_images>();
    auto cmd = ctx.create_command_list();
    auto gpu = gpu_inputs{.ctx = ctx, .cmd = *cmd};
    auto const target = gpu.target();
    auto const group = ctx.transient.create_binding_group(
        *cmd, layout,
        sr::sgl_shaders::svgf_variance_images{
            .history = gpu.texture(in.history).as_texture_view(),
            .moments = gpu.texture(in.moments, sg::pixel_format::rg32_float).as_texture_view(),
            .normal_depth = gpu.texture(in.normal_depth).as_texture_view(),
            .target = target.as_image_view<sg::pixel_format::rgba32_float>()});
    cmd->compute.bind_pipeline(*pipeline);
    cmd->compute.bind_group(0, *group);
    cmd->compute.set_inline_constants(sr::sgl_shaders::svgf_variance_constants{.normal_power = k_normal_power,
                                                                               .depth_sigma = k_depth_sigma,
                                                                               .spatial_below = spatial_below}
                                          .to_block());
    cmd->compute.dispatch_threads(k_w, k_h, 1);
    ctx.submit_command_list(cc::move(cmd));

    auto expected = cc::vector<tg::vec4f>();
    for (auto y = 0; y < k_h; ++y)
        for (auto x = 0; x < k_w; ++x)
        {
            auto const p = y * k_w + x;
            auto const length = f64(in.history[p][3]);
            auto variance = 0.0;
            if (length >= spatial_below)
                variance = cc::max(0.0, f64(in.moments[p][1]) - f64(in.moments[p][0]) * f64(in.moments[p][0]));
            else
            {
                auto m0 = 0.0;
                auto m1 = 0.0;
                auto weight_sum = 0.0;
                for (auto dy = -3; dy <= 3; ++dy)
                    for (auto dx = -3; dx <= 3; ++dx)
                    {
                        if (!inside(x + dx, y + dy))
                            continue;
                        auto const q = (y + dy) * k_w + x + dx;
                        auto const w = geometry_weight(in.normal_depth[p], in.normal_depth[q],
                                                       tg::sqrt(f64(dx * dx + dy * dy)), k_normal_power, k_depth_sigma);
                        m0 += in.moments[q][0] * w;
                        m1 += in.moments[q][1] * w;
                        weight_sum += w;
                    }
                m0 /= weight_sum;
                m1 /= weight_sum;
                variance = cc::max(0.0, m1 - m0 * m0) * spatial_below / cc::max(length, 1.0);
            }
            expected.push_back(tg::vec4f(in.history[p][0], in.history[p][1], in.history[p][2], f32(variance)));
        }
    CHECK(sr_test::worst_stored(sg::pixel_format::rgba32_float, co_await read_back(ctx, target), expected, 4) < 2e-5);
}

// Every channel is compared, so the variance a pass between two others carries forward in alpha is held to the reference too.
ASYNC_INVOCABLE_TEST("sr - svgf's filter pass computes what its description says", (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const in = make_svgf_inputs();
    constexpr auto rgba32 = sg::pixel_format::rgba32_float;

    // the last pass, remodulated, with the caller's alpha; a pass between two others, carrying its variance; the last
    // pass without an albedo, which has nothing to remodulate by
    for (auto const flags : {k_has_albedo | k_remodulate_out | k_has_specular_albedo, k_has_albedo, k_remodulate_out})
    {
        auto const got = co_await run_filter(ctx, in, flags, rgba32);
        CHECK(sr_test::worst_stored(rgba32, got, filter_reference(in, flags), 4) < 2e-5)
            .context(cc::format("flags {:#x}", flags));
    }
}

// The last filter pass writes the caller's image in that image's own format, which the passes before it never do.
ASYNC_INVOCABLE_TEST("sr - svgf's last filter pass stores what it computes into every output format",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const in = make_svgf_inputs();
    constexpr auto flags = k_has_albedo | k_remodulate_out | k_has_specular_albedo;
    auto const expected = filter_reference(in, flags);
    for (auto const format : sr_test::k_swept_formats)
    {
        if (!sg::is_portable_image_format(format) && !ctx.supports(sg::feature::extended_image_formats))
            continue;
        auto const got = co_await run_filter(ctx, in, flags, format);
        CHECK(sr_test::worst_stored(format, got, expected, 4) < 2e-5).context(cc::format("pixel_format {}", int(format)));
    }
}
