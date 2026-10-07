#include "shader_fixtures.hh"

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

// The native denoise members' passes, each run once on the GPU and held to a CPU transcription of what it computes.
//
// The member tests in denoise-test.cc judge what a filter is for: a flat image stays flat, an edge survives, noise goes down.
// None of them would notice a pass that computes something slightly different, such as a floor written 1e-3 where it was 1e-2, or a weight squared where it was not.
// These do, and the reference is written from the pass's description rather than copied from its source, in doubles, so a tolerance covers the GPU's own `exp`, `pow` and `sqrt` and nothing else.
//
// The inputs are deterministic noise with a missed pixel or two in the guides, so every branch of the edge-stops runs.

namespace
{
constexpr int k_w = 23;
constexpr int k_h = 17;

using vec3 = tg::vec<3, f64>;

constexpr auto k_usage
    = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::copy_dst | sg::texture_usage::copy_src;

/// A deterministic value in [0, 1) per index and salt.
[[nodiscard]] f32 noise(int i, int salt)
{
    auto h = u32(i) * 2654435761u ^ u32(salt) * 40503u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return f32(h & 0xffffffu) / 16777216.0f;
}

/// A field of texels, every channel in [lo, hi).
[[nodiscard]] cc::vector<tg::vec4f> field(int salt, f32 lo, f32 hi)
{
    auto out = cc::vector<tg::vec4f>();
    for (auto i = 0; i < k_w * k_h; ++i)
        out.push_back(tg::vec4f(lo + (hi - lo) * noise(i * 4, salt), lo + (hi - lo) * noise(i * 4 + 1, salt),
                                lo + (hi - lo) * noise(i * 4 + 2, salt), lo + (hi - lo) * noise(i * 4 + 3, salt)));
    return out;
}

[[nodiscard]] vec3 xyz(tg::vec4f v)
{
    return vec3(v[0], v[1], v[2]);
}

/// `a` and `b` multiplied channel by channel.
[[nodiscard]] vec3 times(vec3 a, vec3 b)
{
    return vec3(a[0] * b[0], a[1] * b[1], a[2] * b[2]);
}

[[nodiscard]] f64 luminance(vec3 c)
{
    return tg::dot(c, vec3(0.2126, 0.7152, 0.0722));
}

[[nodiscard]] bool inside(int x, int y)
{
    return x >= 0 && y >= 0 && x < k_w && y < k_h;
}

/// The worst disagreement between a pass's output and the reference, relative to the reference's own size.
[[nodiscard]] f64 worst_relative(cc::span<tg::vec4f const> got, cc::span<tg::vec4f const> expected, int channels)
{
    auto worst = 0.0;
    for (auto i = isize(0); i < expected.size(); ++i)
        for (auto c = 0; c < channels; ++c)
            worst = cc::max(worst,
                            tg::abs(f64(got[i][c]) - f64(expected[i][c])) / cc::max(1.0, tg::abs(f64(expected[i][c]))));
    return worst;
}

/// What a test hands the GPU: the textures, created and uploaded on `cmd`.
struct gpu_inputs
{
    sg::context& ctx;
    sg::command_list& cmd;

    [[nodiscard]] sg::texture_2d texture(cc::span<tg::vec4f const> texels,
                                         sg::pixel_format format = sg::pixel_format::rgba32_float)
    {
        auto t = ctx.persistent.create_texture_2d({.format = format, .width = k_w, .height = k_h, .usage = k_usage});
        if (format == sg::pixel_format::rg32_float)
        {
            auto two = cc::vector<tg::vec2f>();
            for (auto const& v : texels)
                two.push_back(tg::vec2f(v[0], v[1]));
            cmd.upload.bytes_to_texture(t.raw(), cc::span<tg::vec2f const>(two).as_bytes());
        }
        else
            cmd.upload.bytes_to_texture(t.raw(), texels.as_bytes());
        return t;
    }

    [[nodiscard]] sg::texture_2d target(sg::pixel_format format = sg::pixel_format::rgba32_float)
    {
        return ctx.persistent.create_texture_2d({.format = format, .width = k_w, .height = k_h, .usage = k_usage});
    }
};

/// Reads `t` back as float4 texels, widening a two-channel image with zeros.
[[nodiscard]] cc::shared_async<cc::vector<tg::vec4f>> read_back(sg::context& ctx, sg::texture_2d const& t)
{
    auto cmd = ctx.create_command_list();
    auto const bytes = cmd->download.bytes_from_texture(t.raw());
    ctx.submit_command_list(cc::move(cmd));
    ctx.advance_epoch();
    auto const& data = co_await bytes.bytes();
    auto out = cc::vector<tg::vec4f>();
    auto const two = t.format() == sg::pixel_format::rg32_float;
    for (auto i = 0; i < k_w * k_h; ++i)
    {
        auto v = tg::vec4f(0, 0, 0, 0);
        cc::memcpy(&v, data.data() + i * (two ? 8 : 16), two ? 8 : 16);
        out.push_back(v);
    }
    co_return out;
}

[[nodiscard]] cc::shared_async<sg::compute_pipeline_handle> settled(sg::async_compute_pipeline pipeline)
{
    co_await cc::async_settled(pipeline);
    auto const* const built = pipeline->try_value();
    REQUIRE(built != nullptr).context("the pass did not build");
    co_return *built;
}

// ---- the references -------------------------------------------------------------------------------------------------

constexpr f64 k_kernel[5] = {1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0};

struct atrous_case
{
    int step = 1;
    u32 flags = 0;
    f64 luminance_scale = 1.0;
    f64 normal_power = 64.0;
    f64 depth_sigma = 0.02;
};

/// One à-trous pass, as atrous_denoise.sgl describes it.
[[nodiscard]] cc::vector<tg::vec4f> atrous_reference(atrous_case const& k,
                                                     cc::span<tg::vec4f const> source,
                                                     cc::span<tg::vec4f const> albedo,
                                                     cc::span<tg::vec4f const> specular,
                                                     cc::span<tg::vec4f const> normal,
                                                     cc::span<tg::vec4f const> depth,
                                                     cc::span<tg::vec4f const> counts)
{
    auto const is = [&](u32 bit) { return (k.flags & bit) != 0; };
    auto const albedo_at = [&](int i)
    {
        auto a = xyz(albedo[i]);
        if (is(32u))
            a += xyz(specular[i]);
        return vec3(cc::max(a[0], 1e-3), cc::max(a[1], 1e-3), cc::max(a[2], 1e-3));
    };
    auto const source_at = [&](int i)
    {
        auto c = xyz(source[i]);
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
            auto const n_p = xyz(normal[p]);
            auto const center_has_normal = is(2u) && tg::dot(n_p, n_p) > 0.0;
            auto const center_depth = is(4u) ? f64(depth[p][0]) : 0.0;
            auto scale = k.luminance_scale;
            if (is(64u))
                scale /= tg::sqrt(cc::max(f64(counts[p][0]), 1.0));
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
                        auto const n_q = xyz(normal[q]);
                        auto const tap_has_normal = tg::dot(n_q, n_q) > 0.0;
                        if (center_has_normal != tap_has_normal)
                            w = 0.0;
                        else if (center_has_normal)
                            w *= tg::pow(cc::clamp(tg::dot(tg::normalize(n_p), tg::normalize(n_q)), 0.0, 1.0),
                                         k.normal_power);
                    }
                    if (is(4u))
                    {
                        auto const d = f64(depth[q][0]);
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
            out.push_back(tg::vec4f(f32(result[0]), f32(result[1]), f32(result[2]), source[p][3]));
        }
    return out;
}

[[nodiscard]] f64 svgf_reflectance_component(f64 albedo, f64 specular, bool with_specular)
{
    return cc::max(with_specular ? albedo + specular : albedo, 1e-3);
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
} // namespace

// One pass of the plain à-trous member with every guide, and with none: the edge-stops, the demodulation round trip and the per-pixel sample count are each in the result or the comparison fails.
ASYNC_INVOCABLE_TEST("sr - an a-trous pass computes what its description says, guide by guide",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const source = field(1, 0.0f, 3.0f);
    auto const albedo = field(2, 0.0f, 1.0f);
    auto const specular = field(3, 0.0f, 0.5f);
    auto normal = field(4, -1.0f, 1.0f);
    normal[5] = tg::vec4f(0, 0, 0, 0); // a missed pixel among the normals
    auto depth = field(5, 0.5f, 1.5f);
    depth[7] = tg::vec4f(-1, 0, 0, 0); // and two among the depths
    depth[40] = tg::vec4f(0, 0, 0, 0);
    auto const counts = field(6, 1.0f, 64.0f);

    auto const values = sr::sgl_shaders::atrous_denoise_options{.target_format = sg::pixel_format::rgba32_float};
    auto const pipeline = co_await settled(sr::sgl_shaders::atrous_denoise.main_cs.acquire_pipeline(ctx, values));
    auto const layout = ctx.cached.acquire_binding_group_layout(
        sr::sgl_shaders::atrous_images::declared_bindings(values), sr::sgl_shaders::atrous_images::declared_samplers());

    // every flag, then no guide at all, then the guides without the albedo round trip
    for (auto const k : {atrous_case{.step = 2, .flags = 0x7fu, .luminance_scale = 0.7},
                         atrous_case{.step = 1, .flags = 0u, .luminance_scale = 1.5},
                         atrous_case{.step = 4, .flags = 0x46u, .luminance_scale = 0.4, .depth_sigma = 0.3}})
    {
        auto cmd = ctx.create_command_list();
        auto in = gpu_inputs{.ctx = ctx, .cmd = *cmd};
        auto const target = in.target();
        auto const group = ctx.transient.create_binding_group(
            *cmd, layout,
            sr::sgl_shaders::atrous_images{.source = in.texture(source).as_texture_view(),
                                           .albedo = in.texture(albedo).as_texture_view(),
                                           .specular_albedo = in.texture(specular).as_texture_view(),
                                           .normal = in.texture(normal).as_texture_view(),
                                           .depth = in.texture(depth).as_texture_view(),
                                           .sample_counts = in.texture(counts).as_texture_view(),
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

        auto const got = co_await read_back(ctx, target);
        auto const expected = atrous_reference(k, source, albedo, specular, normal, depth, counts);
        CHECK(worst_relative(got, expected, 4) < 2e-5).context(cc::format("flags {:#x}", k.flags));
    }
}

// SVGF's three passes, each from inputs of its own, so one wrong pass cannot hide behind another.
ASYNC_INVOCABLE_TEST("sr - svgf's temporal, variance and filter passes compute what their descriptions say",
                     (sg::context_handle const& ctx_h))
{
    REQUIRE(ctx_h != nullptr);
    auto& ctx = *ctx_h;
    (void)sr_test::shader_fixtures(); // sr's one library, alive for the whole binary

    auto const color = field(11, 0.0f, 3.0f);
    auto const albedo = field(12, 0.0f, 1.0f);
    auto const specular = field(13, 0.0f, 0.5f);
    auto normal = field(14, -1.0f, 1.0f);
    normal[3] = tg::vec4f(0, 0, 0, 0);
    auto depth = field(15, 0.5f, 1.5f);
    depth[9] = tg::vec4f(0, 0, 0, 0);
    auto const motion = field(16, -1.5f, 1.5f);
    auto history = field(17, 0.0f, 2.0f);
    for (auto i = 0; i < k_w * k_h; ++i)
        history[i][3] = f32(1 + i % 7); // a history length in alpha, young and old pixels mixed
    auto const moments = field(18, 0.0f, 1.0f);
    auto normal_depth = field(19, -1.0f, 1.0f);
    for (auto i = 0; i < k_w * k_h; ++i)
        normal_depth[i][3] = 0.6f + 0.3f * noise(i, 99);
    normal_depth[11] = tg::vec4f(0, 0, 0, 0);

    // -- temporal, with the albedo pair and the history taken where the surface matches
    {
        constexpr auto flags = 0x9u;
        constexpr auto color_alpha_min = 0.05;
        constexpr auto moments_alpha_min = 0.2;
        constexpr auto max_history = 4.0;
        constexpr auto normal_similarity = 0.2;
        constexpr auto depth_similarity = 0.5;

        auto const pipeline = co_await settled(sr::sgl_shaders::svgf.temporal_cs.acquire_pipeline(ctx));
        auto const layout = ctx.cached.acquire_binding_group_layout<sr::sgl_shaders::svgf_temporal_images>();
        auto cmd = ctx.create_command_list();
        auto in = gpu_inputs{.ctx = ctx, .cmd = *cmd};
        auto const out_history = in.target();
        auto const out_moments = in.target(sg::pixel_format::rg32_float);
        auto const out_normal_depth = in.target();
        auto const group = ctx.transient.create_binding_group(
            *cmd, layout,
            sr::sgl_shaders::svgf_temporal_images{
                .color = in.texture(color).as_texture_view(),
                .albedo = in.texture(albedo).as_texture_view(),
                .specular_albedo = in.texture(specular).as_texture_view(),
                .normal = in.texture(normal).as_texture_view(),
                .depth = in.texture(depth).as_texture_view(),
                .motion = in.texture(motion).as_texture_view(),
                .previous_history = in.texture(history).as_texture_view(),
                .previous_moments = in.texture(moments, sg::pixel_format::rg32_float).as_texture_view(),
                .previous_normal_depth = in.texture(normal_depth).as_texture_view(),
                .history = out_history.as_image_view<sg::pixel_format::rgba32_float>(),
                .moments = out_moments.as_image_view<sg::pixel_format::rg32_float>(),
                .normal_depth = out_normal_depth.as_image_view<sg::pixel_format::rgba32_float>()});
        cmd->compute.bind_pipeline(*pipeline);
        cmd->compute.bind_group(0, *group);
        cmd->compute.set_inline_constants(sr::sgl_shaders::svgf_temporal_constants{.flags = flags,
                                                                                   .color_alpha_min = color_alpha_min,
                                                                                   .moments_alpha_min = moments_alpha_min,
                                                                                   .max_history = max_history,
                                                                                   .normal_similarity = normal_similarity,
                                                                                   .depth_similarity = depth_similarity}
                                              .to_block());
        cmd->compute.dispatch_threads(k_w, k_h, 1);
        ctx.submit_command_list(cc::move(cmd));

        auto expected_history = cc::vector<tg::vec4f>();
        auto expected_moments = cc::vector<tg::vec4f>();
        auto taken = 0;
        for (auto y = 0; y < k_h; ++y)
            for (auto x = 0; x < k_w; ++x)
            {
                auto const p = y * k_w + x;
                auto c = xyz(color[p]);
                for (auto i = 0; i < 3; ++i)
                    c[i] /= svgf_reflectance_component(albedo[p][i], specular[p][i], true);
                auto const n = xyz(normal[p]);
                auto const d = f64(depth[p][0]);
                auto const lum = luminance(c);
                auto const qx = int(tg::floor(f64(f32(f32(x) + 0.5f - motion[p][0]))));
                auto const qy = int(tg::floor(f64(f32(f32(y) + 0.5f - motion[p][1]))));
                auto valid = inside(qx, qy);
                auto const q = valid ? qy * k_w + qx : 0;
                if (valid)
                {
                    auto const prev = normal_depth[q];
                    auto const surface = is_surface(n, d);
                    if (surface != is_surface(xyz(prev), prev[3]))
                        valid = false;
                    else if (surface)
                        valid = tg::dot(tg::normalize(n), tg::normalize(xyz(prev))) >= normal_similarity
                             && tg::abs(f64(prev[3]) - d) / cc::max(d, 1e-4) <= depth_similarity;
                }
                auto out_color = c;
                auto out_m0 = lum;
                auto out_m1 = lum * lum;
                auto length = 1.0;
                if (valid)
                {
                    ++taken;
                    length = cc::min(f64(history[q][3]) + 1.0, max_history);
                    auto const ca = cc::max(1.0 / length, color_alpha_min);
                    auto const ma = cc::max(1.0 / length, moments_alpha_min);
                    out_color = xyz(history[q]) + (c - xyz(history[q])) * ca;
                    out_m0 = moments[q][0] + (lum - moments[q][0]) * ma;
                    out_m1 = moments[q][1] + (lum * lum - moments[q][1]) * ma;
                }
                expected_history.push_back(
                    tg::vec4f(f32(out_color[0]), f32(out_color[1]), f32(out_color[2]), f32(length)));
                expected_moments.push_back(tg::vec4f(f32(out_m0), f32(out_m1), 0, 0));
            }
        // both kinds of pixel are present, or the comparison says nothing about one of them
        CHECK(taken > 0);
        CHECK(taken < k_w * k_h);

        CHECK(worst_relative(co_await read_back(ctx, out_history), expected_history, 4) < 2e-5).context("history");
        CHECK(worst_relative(co_await read_back(ctx, out_moments), expected_moments, 2) < 2e-5).context("moments");
        auto expected_normal_depth = cc::vector<tg::vec4f>();
        for (auto i = 0; i < k_w * k_h; ++i)
            expected_normal_depth.push_back(tg::vec4f(normal[i][0], normal[i][1], normal[i][2], depth[i][0]));
        CHECK(worst_relative(co_await read_back(ctx, out_normal_depth), expected_normal_depth, 4) == 0.0)
            .context("normal_depth");
    }

    // -- variance, young pixels estimated spatially and old ones read off their moments
    {
        constexpr auto normal_power = 64.0;
        constexpr auto depth_sigma = 0.5;
        constexpr auto spatial_below = 4.0;

        auto const pipeline = co_await settled(sr::sgl_shaders::svgf.variance_cs.acquire_pipeline(ctx));
        auto const layout = ctx.cached.acquire_binding_group_layout<sr::sgl_shaders::svgf_variance_images>();
        auto cmd = ctx.create_command_list();
        auto in = gpu_inputs{.ctx = ctx, .cmd = *cmd};
        auto const target = in.target();
        auto const group = ctx.transient.create_binding_group(
            *cmd, layout,
            sr::sgl_shaders::svgf_variance_images{
                .history = in.texture(history).as_texture_view(),
                .moments = in.texture(moments, sg::pixel_format::rg32_float).as_texture_view(),
                .normal_depth = in.texture(normal_depth).as_texture_view(),
                .target = target.as_image_view<sg::pixel_format::rgba32_float>()});
        cmd->compute.bind_pipeline(*pipeline);
        cmd->compute.bind_group(0, *group);
        cmd->compute.set_inline_constants(sr::sgl_shaders::svgf_variance_constants{.normal_power = normal_power,
                                                                                   .depth_sigma = depth_sigma,
                                                                                   .spatial_below = spatial_below}
                                              .to_block());
        cmd->compute.dispatch_threads(k_w, k_h, 1);
        ctx.submit_command_list(cc::move(cmd));

        auto expected = cc::vector<tg::vec4f>();
        for (auto y = 0; y < k_h; ++y)
            for (auto x = 0; x < k_w; ++x)
            {
                auto const p = y * k_w + x;
                auto const length = f64(history[p][3]);
                auto variance = 0.0;
                if (length >= spatial_below)
                    variance = cc::max(0.0, f64(moments[p][1]) - f64(moments[p][0]) * f64(moments[p][0]));
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
                            auto const w = geometry_weight(normal_depth[p], normal_depth[q],
                                                           tg::sqrt(f64(dx * dx + dy * dy)), normal_power, depth_sigma);
                            m0 += moments[q][0] * w;
                            m1 += moments[q][1] * w;
                            weight_sum += w;
                        }
                    m0 /= weight_sum;
                    m1 /= weight_sum;
                    variance = cc::max(0.0, m1 - m0 * m0) * spatial_below / cc::max(length, 1.0);
                }
                expected.push_back(tg::vec4f(history[p][0], history[p][1], history[p][2], f32(variance)));
            }
        CHECK(worst_relative(co_await read_back(ctx, target), expected, 4) < 2e-5).context("variance");
    }

    // -- one filter pass, the last one: remodulated, with the caller's alpha
    {
        constexpr auto step = 2;
        constexpr auto flags = 0xdu;
        constexpr auto luminance_sigma = 4.0;
        constexpr auto normal_power = 64.0;
        constexpr auto depth_sigma = 0.5;

        auto const values = sr::sgl_shaders::svgf_options{.target_format = sg::pixel_format::rgba32_float};
        auto const pipeline = co_await settled(sr::sgl_shaders::svgf.atrous_cs.acquire_pipeline(ctx, values));
        auto const layout
            = ctx.cached.acquire_binding_group_layout(sr::sgl_shaders::svgf_atrous_images::declared_bindings(values),
                                                      sr::sgl_shaders::svgf_atrous_images::declared_samplers());
        auto cmd = ctx.create_command_list();
        auto in = gpu_inputs{.ctx = ctx, .cmd = *cmd};
        auto const target = in.target();
        auto const group = ctx.transient.create_binding_group(
            *cmd, layout,
            sr::sgl_shaders::svgf_atrous_images{.source = in.texture(history).as_texture_view(),
                                                .normal_depth = in.texture(normal_depth).as_texture_view(),
                                                .albedo = in.texture(albedo).as_texture_view(),
                                                .specular_albedo = in.texture(specular).as_texture_view(),
                                                .color = in.texture(color).as_texture_view(),
                                                .target = target.as_any_image_view()});
        cmd->compute.bind_pipeline(*pipeline);
        cmd->compute.bind_group(0, *group);
        cmd->compute.set_inline_constants(sr::sgl_shaders::svgf_atrous_constants{.step = step,
                                                                                 .flags = flags,
                                                                                 .luminance_sigma = luminance_sigma,
                                                                                 .normal_power = normal_power,
                                                                                 .depth_sigma = depth_sigma}
                                              .to_block());
        cmd->compute.dispatch_threads(k_w, k_h, 1);
        ctx.submit_command_list(cc::move(cmd));

        auto expected = cc::vector<tg::vec4f>();
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
                        blurred += history[qy * k_w + qx][3] * w;
                        blur_weight += w;
                    }
                blurred /= blur_weight;
                auto const lum_width = luminance_sigma * tg::sqrt(cc::max(blurred, 0.0)) + 1e-6;
                auto const center_lum = luminance(xyz(history[p]));

                auto sum = vec3(0, 0, 0);
                auto variance_sum = 0.0;
                auto weight_sum = 0.0;
                for (auto dy = -2; dy <= 2; ++dy)
                    for (auto dx = -2; dx <= 2; ++dx)
                    {
                        auto const qx = x + dx * step;
                        auto const qy = y + dy * step;
                        if (!inside(qx, qy))
                            continue;
                        auto const q = qy * k_w + qx;
                        auto const length = tg::sqrt(f64(dx * dx + dy * dy)) * step;
                        auto w = k_kernel[dx + 2] * k_kernel[dy + 2]
                               * geometry_weight(normal_depth[p], normal_depth[q], length, normal_power, depth_sigma);
                        w *= tg::exp(-tg::abs(luminance(xyz(history[q])) - center_lum) / lum_width);
                        sum += xyz(history[q]) * w;
                        variance_sum += history[q][3] * w * w;
                        weight_sum += w;
                    }
                auto result = sum / weight_sum;
                for (auto i = 0; i < 3; ++i)
                    result[i] *= svgf_reflectance_component(albedo[p][i], specular[p][i], true);
                expected.push_back(tg::vec4f(f32(result[0]), f32(result[1]), f32(result[2]), color[p][3]));
            }
        CHECK(worst_relative(co_await read_back(ctx, target), expected, 4) < 2e-5).context("filter");
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
