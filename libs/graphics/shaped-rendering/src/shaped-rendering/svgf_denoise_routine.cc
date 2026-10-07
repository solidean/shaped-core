#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/svgf_denoise_routine.hh>
#include <sr_sgl_shaders.hh>

namespace sr
{
using impl::extent_of;
using impl::is_set;

namespace
{
// The flag bits svgf.sgl declares.
constexpr u32 k_has_albedo = 1u << 0;
constexpr u32 k_reset = 1u << 1;
constexpr u32 k_remodulate_out = 1u << 2;
constexpr u32 k_has_specular_albedo = 1u << 3;

// Where each image lives in the history's state slots.
// The three histories ping-pong on the frame's parity: this call writes slot `base + parity` and reads the other.
constexpr int k_scratch = 0;      // 0, 1: the à-trous passes' ping-pong
constexpr int k_color = 2;        // 2, 3: demodulated colour, history length in alpha
constexpr int k_moments = 4;      // 4, 5: luminance mean and mean square
constexpr int k_normal_depth = 6; // 6, 7: the guides as the frame that wrote the history saw them
constexpr int k_slots_used = reconstruct_history::state_slots; // svgf fills the history, so a ninth would grow it

static_assert(k_normal_depth + 2 == k_slots_used, "svgf's slot map leaves a gap or runs past the history's state");
static_assert(k_slots_used == reconstruct_history::state_slots, "svgf no longer fills the history it was sized against");

/// What the history images are, and what the filter's passes between the first and the last write.
/// Full floats: the moments square the luminance, and the color history divides by the albedo.
/// That is the range a half float loses at the dim end.
constexpr auto k_history_format = sg::pixel_format::rgba32_float;
constexpr auto k_moments_format = sg::pixel_format::rg32_float;
} // namespace

svgf_options svgf_denoise_routine::options_for(reconstruct_settings const& settings)
{
    auto options = svgf_options{};
    switch (settings.quality)
    {
    case denoise_quality::fast:
        options.iterations = 3;
        break;
    case denoise_quality::balanced:
        options.iterations = 4;
        break;
    case denoise_quality::best:
        options.iterations = 5;
        break;
    }
    // Sharpness 0 averages across eight measured noise widths, 1 across two.
    auto const sharpness = cc::clamp(settings.sharpness, 0.0f, 1.0f);
    options.luminance_sigma = 8.0f + (2.0f - 8.0f) * sharpness;

    // Responsiveness 0 keeps a long history (a new frame weighs 1/32), 1 hands every frame half the weight.
    auto const responsiveness = cc::clamp(settings.temporal_responsiveness, 0.0f, 1.0f);
    options.color_alpha_min = 1.0f / 32.0f + (0.5f - 1.0f / 32.0f) * responsiveness;
    options.moments_alpha_min = cc::max(options.color_alpha_min, 0.2f);
    return options;
}

cc::shared_async<cc::unit> svgf_denoise_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // Cleared first, so a reload that fails to compile leaves nothing built against the previous shaders.
    _temporal = {};
    _variance = {};
    _atrous = {};
    _atrous_output = {};

    // The filter is two compiles of one entry point, unless the two formats agree.
    // The passes between write the scratch images, and the last the caller's output.
    auto const atrous_pass = [&](sg::pixel_format target)
    {
        auto const values = sgl_shaders::svgf_options{.target_format = target};
        return pass{.group_layout
                    = ctx.cached.acquire_binding_group_layout(sgl_shaders::svgf_atrous_images::declared_bindings(values),
                                                              sgl_shaders::svgf_atrous_images::declared_samplers()),
                    .pipeline = sgl_shaders::svgf.atrous_cs.acquire_pipeline(ctx, values)};
    };

    // All four compile at once; settling them one after another costs no concurrency.
    auto const temporal
        = pass{.group_layout = ctx.cached.acquire_binding_group_layout<sgl_shaders::svgf_temporal_images>(),
               .pipeline = sgl_shaders::svgf.temporal_cs.acquire_pipeline(ctx)};
    auto const variance
        = pass{.group_layout = ctx.cached.acquire_binding_group_layout<sgl_shaders::svgf_variance_images>(),
               .pipeline = sgl_shaders::svgf.variance_cs.acquire_pipeline(ctx)};
    auto const atrous = atrous_pass(k_history_format);
    auto const atrous_output = params() == k_history_format ? atrous : atrous_pass(params());

    for (auto const* const p : {&temporal, &variance, &atrous, &atrous_output})
    {
        co_await cc::async_settled(p->pipeline);
        if (p->pipeline->try_value() == nullptr)
        {
            fail_init(); // not pending: this will not come good until a reload, and a caller should be able to tell
            co_return;
        }
    }

    // Published together, after the build: readiness is what makes them visible to execute.
    _temporal = temporal;
    _variance = variance;
    _atrous = atrous;
    _atrous_output = atrous_output;
    co_return;
}

reconstruct_outcome svgf_denoise_routine::execute(sg::command_list& cmd,
                                                  reconstruct_inputs const& in,
                                                  reconstruct_history& history,
                                                  svgf_options const& options)
{
    CC_ASSERT(is_set(in.color) && is_set(in.output), "a denoise call needs a colour and an output texture");
    CC_ASSERT(in.output.raw() != in.color.raw(), "SVGF needs an output texture other than its input");
    CC_ASSERT(extent_of(in.output) == extent_of(in.color), "SVGF does not upscale: output and input extents differ");
    CC_ASSERT(is_set(in.guides.normal) && is_set(in.guides.depth) && is_set(in.guides.motion),
              "SVGF needs the normal, depth and motion guides");
    CC_ASSERT(options.iterations >= 1, "SVGF needs at least one filter pass");

    // Read-only: execute touches nothing init did not publish, and every image it writes is the caller's.
    auto const self = try_acquire(cmd, in.output.format());
    if (self.is_pending())
        return {.status = reconstruct_status::pending, .denoiser = denoise_method::svgf};
    if (self.is_failed())
        return {.status = reconstruct_status::failed, .denoiser = denoise_method::svgf};

    auto& ctx = cmd.context();
    auto const extent = extent_of(in.color);
    auto const restarted = history._prepare(denoise_method::svgf, extent);

    // The moments pair carries two channels and is allocated as two — 221 MiB per 1080p stream rather than 253.
    auto created = false;
    for (auto i = 0; i < k_slots_used; ++i)
    {
        auto const is_moments = i == k_moments || i == k_moments + 1;
        auto const format = is_moments ? k_moments_format : k_history_format;
        created = impl::ensure_image(ctx, history._state[i], extent, format) || created;
    }

    // A history image that was just created holds nothing, so it must not be read as one.
    auto const reset = restarted || created;

    auto const cur = int(history._frame & 1u);
    auto const prev = 1 - cur;
    auto const& s = history._state;

    auto const has_albedo = is_set(in.guides.albedo);
    auto const albedo = has_albedo ? in.guides.albedo.as_texture_view() : in.color.as_texture_view();
    auto const has_specular_albedo = is_set(in.guides.specular_albedo);
    auto const specular_albedo
        = has_specular_albedo ? in.guides.specular_albedo.as_texture_view() : in.color.as_texture_view();
    auto const albedo_flag = (has_albedo ? k_has_albedo : 0u) | (has_specular_albedo ? k_has_specular_albedo : 0u);

    // -- temporal
    {
        auto const group = ctx.transient.create_binding_group(
            cmd, self->_temporal.group_layout,
            sgl_shaders::svgf_temporal_images{
                .color = in.color.as_texture_view(),
                .albedo = albedo,
                .specular_albedo = specular_albedo,
                .normal = in.guides.normal.as_texture_view(),
                .depth = in.guides.depth.as_texture_view(),
                .motion = in.guides.motion.as_texture_view(),
                .previous_history = s[k_color + prev].as_texture_view(),
                .previous_moments = s[k_moments + prev].as_texture_view(),
                .previous_normal_depth = s[k_normal_depth + prev].as_texture_view(),
                .history = s[k_color + cur].as_image_view<k_history_format>(),
                .moments = s[k_moments + cur].as_image_view<k_moments_format>(),
                .normal_depth = s[k_normal_depth + cur].as_image_view<k_history_format>(),
            });
        cmd.compute.bind_pipeline(**self->_temporal.pipeline->try_value());
        cmd.compute.bind_group(0, *group);
        cmd.compute.set_inline_constants(sgl_shaders::svgf_temporal_constants{
            .flags = albedo_flag | (reset ? k_reset : 0u),
            .color_alpha_min = options.color_alpha_min,
            .moments_alpha_min = options.moments_alpha_min,
            .max_history = options.max_history,
            .normal_similarity = options.normal_similarity,
            .depth_similarity = options.depth_similarity,
        }
                                             .to_block());
        cmd.compute.dispatch_threads(extent[0], extent[1], 1);
    }

    // -- variance, into the first scratch image
    {
        auto const group
            = ctx.transient.create_binding_group(cmd, self->_variance.group_layout,
                                                 sgl_shaders::svgf_variance_images{
                                                     .history = s[k_color + cur].as_texture_view(),
                                                     .moments = s[k_moments + cur].as_texture_view(),
                                                     .normal_depth = s[k_normal_depth + cur].as_texture_view(),
                                                     .target = s[k_scratch].as_image_view<k_history_format>(),
                                                 });
        cmd.compute.bind_pipeline(**self->_variance.pipeline->try_value());
        cmd.compute.bind_group(0, *group);
        cmd.compute.set_inline_constants(sgl_shaders::svgf_variance_constants{
            .normal_power = options.normal_power,
            .depth_sigma = options.depth_sigma,
            .spatial_below = options.spatial_variance_below,
        }
                                             .to_block());
        cmd.compute.dispatch_threads(extent[0], extent[1], 1);
    }

    // -- à-trous: pass i reads scratch[i % 2] and writes the other, the last one the output
    auto const last = options.iterations - 1;
    for (auto i = 0; i <= last; ++i)
    {
        auto const& source = s[k_scratch + i % 2];
        auto const& target = i == last ? in.output : s[k_scratch + (i + 1) % 2];

        auto const& pass = i == last ? self->_atrous_output : self->_atrous;
        auto const group
            = ctx.transient.create_binding_group(cmd, pass.group_layout,
                                                 sgl_shaders::svgf_atrous_images{
                                                     .source = source.as_texture_view(),
                                                     .normal_depth = s[k_normal_depth + cur].as_texture_view(),
                                                     .albedo = albedo,
                                                     .specular_albedo = specular_albedo,
                                                     .color = in.color.as_texture_view(),
                                                     .target = target.as_any_image_view(),
                                                 });
        cmd.compute.bind_pipeline(**pass.pipeline->try_value());
        cmd.compute.bind_group(0, *group);
        cmd.compute.set_inline_constants(sgl_shaders::svgf_atrous_constants{
            .step = 1 << i,
            .flags = albedo_flag | (i == last ? k_remodulate_out : 0u),
            .luminance_sigma = options.luminance_sigma,
            .normal_power = options.normal_power,
            .depth_sigma = options.depth_sigma,
        }
                                             .to_block());
        cmd.compute.dispatch_threads(extent[0], extent[1], 1);
    }

    ++history._frame;
    return {.status = reconstruct_status::denoised, .denoiser = denoise_method::svgf, .restarted = reset};
}
} // namespace sr
