#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/atrous_denoise_routine.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <sr_shaders.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace sr
{
using impl::extent_of;
using impl::is_set;

namespace
{
// The flag bits atrous_denoise.hlsl tests.
constexpr u32 k_has_albedo = 1u << 0;
constexpr u32 k_has_normal = 1u << 1;
constexpr u32 k_has_depth = 1u << 2;
constexpr u32 k_demodulate_in = 1u << 3;
constexpr u32 k_remodulate_out = 1u << 4;

// Where this member's images live in the history's state slots.
// à-trous keeps no history, so the two scratch slots are all it ever touches — the rest stay empty under it.
constexpr int k_scratch = 0; // 0, 1: the passes' ping-pong
constexpr int k_slots_used = 2;

static_assert(k_scratch + k_slots_used <= denoise_history::state_slots,
              "a-trous reaches past the slots denoise_history has");

/// Each pass averages what the last one left, so the noise it has to see through shrinks.
/// Halving the variance per pass is the usual approximation when no variance estimate is carried along.
constexpr f32 k_per_pass_noise_falloff = 0.70710678f;

} // namespace

atrous_options atrous_denoise_routine::options_for(denoise_settings const& settings)
{
    auto options = atrous_options{};
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
    // Sharpness 0 averages across luminance differences of four noise widths, 1 across half of one.
    auto const sharpness = cc::clamp(settings.sharpness, 0.0f, 1.0f);
    options.luminance_sigma = 4.0f + (0.5f - 4.0f) * sharpness;
    return options;
}

cc::shared_async<cc::unit> atrous_denoise_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // Cleared first, so a reload that fails to compile leaves nothing built against the previous shader.
    _group_layout = nullptr;
    _pipeline = nullptr;

    auto const shader = sr::shaders::atrous_denoise.compute.main_cs->acquire(ctx);
    co_await cc::async_settled(shader);
    auto const* const compiled = shader->try_value();
    if (compiled == nullptr)
    {
        fail_init();
        co_return;
    }

    auto const* const constants_binding = [&]() -> sg::binding const*
    {
        for (auto const& b : compiled->bindings)
            if (b.type == sg::binding_type::constants_buffer)
                return &b;
        return nullptr;
    }();
    if (constants_binding == nullptr)
    {
        fail_init(); // a shader that reflects no constants block cannot be driven
        co_return;
    }

    auto group_layout = ctx.cached.acquire_binding_group_layout<shaders::atrous_bindings>();
    auto const pipeline_layout
        = ctx.cached.acquire_pipeline_layout({.groups = {group_layout}, .inline_constants = *constants_binding});

    auto const pipeline = ctx.cached.acquire_compute_pipeline({.shader = *compiled, .layout = pipeline_layout});
    co_await cc::async_settled(pipeline);
    auto const* const built = pipeline->try_value();
    if (built == nullptr)
    {
        fail_init();
        co_return;
    }

    // Both published together, after the build: readiness is what makes them visible to execute.
    _group_layout = cc::move(group_layout);
    _pipeline = *built;
    co_return;
}

denoise_outcome atrous_denoise_routine::execute(sg::command_list& cmd,
                                                denoise_inputs const& in,
                                                denoise_history& history,
                                                atrous_options const& options)
{
    CC_ASSERT(is_set(in.color), "a denoise call needs a colour texture");
    CC_ASSERT(is_set(in.output), "a denoise call needs an output texture");
    CC_ASSERT(in.output.raw() != in.color.raw(), "à-trous needs an output texture other than its input");
    CC_ASSERT(extent_of(in.output) == extent_of(in.color), "à-trous does not upscale: output and input extents differ");
    CC_ASSERT(options.iterations >= 1, "à-trous needs at least one pass");

    // Read-only: execute touches nothing init did not publish, and the ping-pong images are the caller's.
    auto const self = try_acquire(cmd);
    if (self.is_pending())
        return {.status = denoise_status::pending, .method = denoise_method::atrous};
    if (self.is_failed())
        return {.status = denoise_status::failed, .method = denoise_method::atrous};

    auto& ctx = cmd.context();
    auto const extent = extent_of(in.color);
    auto const restarted = history._prepare(denoise_method::atrous, extent);

    // Pass i reads scratch[(i - 1) % 2] and writes scratch[i % 2], so no pass reads what it writes.
    // The first reads the input and the last writes the output, so one pass needs no scratch and two need one image.
    // rgba32_float to match the accumulator it most often filters: the first pass divides by the albedo, and a half
    // float would lose the dim end of that range.
    if (options.iterations > 1)
        (void)impl::ensure_image(ctx, history._state[k_scratch], extent, sg::pixel_format::rgba32_float);
    if (options.iterations > 2)
        (void)impl::ensure_image(ctx, history._state[k_scratch + 1], extent, sg::pixel_format::rgba32_float);

    auto guide_flags = u32(0);
    if (is_set(in.guides.albedo))
        guide_flags |= k_has_albedo;
    if (is_set(in.guides.normal))
        guide_flags |= k_has_normal;
    if (is_set(in.guides.depth))
        guide_flags |= k_has_depth;
    auto const demodulate = options.demodulate_albedo && is_set(in.guides.albedo);

    // A missing guide is bound to the colour texture as a stand-in, so the group has every slot filled; its flag is
    // clear, so the shader never reads it.
    auto const stand_in
        = [&](sg::texture_2d const& t) { return is_set(t) ? t.as_texture_view() : in.color.as_texture_view(); };
    auto const albedo = stand_in(in.guides.albedo);
    auto const normal = stand_in(in.guides.normal);
    auto const depth = stand_in(in.guides.depth);

    auto const sample_count = in.sample_count == 0 ? 1u : in.sample_count;
    auto luminance_scale = options.luminance_sigma / tg::sqrt(f32(sample_count));

    auto const last = options.iterations - 1;
    for (auto i = 0; i <= last; ++i)
    {
        auto const& source = i == 0 ? in.color : history._state[k_scratch + (i - 1) % 2];
        auto const& target = i == last ? in.output : history._state[k_scratch + i % 2];

        auto flags = guide_flags;
        if (demodulate && i == 0)
            flags |= k_demodulate_in;
        if (demodulate && i == last)
            flags |= k_remodulate_out;

        auto const constants = shaders::atrous_constants{
            .step = 1 << i,
            .flags = flags,
            .luminance_scale = luminance_scale,
            .normal_power = options.normal_power,
            .depth_sigma = options.depth_sigma,
        };

        auto const group = ctx.transient.create_binding_group(cmd, self->_group_layout,
                                                              shaders::atrous_bindings{
                                                                  .gSource = source.as_texture_view(),
                                                                  .gAlbedo = albedo,
                                                                  .gNormal = normal,
                                                                  .gDepth = depth,
                                                                  .gTarget = target.as_any_image_view(),
                                                              });

        cmd.compute.bind_pipeline(*self->_pipeline);
        cmd.compute.bind<shaders::atrous_bindings>(*group);
        cmd.compute.set_inline_constants(constants);
        cmd.compute.dispatch_threads(extent[0], extent[1], 1);

        luminance_scale *= k_per_pass_noise_falloff;
    }

    return {.status = denoise_status::denoised, .method = denoise_method::atrous, .restarted = restarted};
}
} // namespace sr
