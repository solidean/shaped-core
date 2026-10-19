#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/impl/metalfx_backend.hh>
#include <shaped-rendering/metalfx_rr_routine.hh>
#include <sr_sgl_shaders.hh>

#include <memory> // std::shared_ptr, which is what reconstruct_history::_member_state is

namespace sr
{
using impl::extent_of;
using impl::is_set;

namespace
{
/// A stream's denoised scaler, the formats it was created for, and the images around it.
struct metalfx_rr_state
{
    std::shared_ptr<impl::metalfx_denoised_stream> stream;
    impl::metalfx_denoised_stream_desc desc;
    impl::metalfx_images images;
};

[[nodiscard]] bool same_formats(impl::metalfx_denoised_stream_desc const& a, impl::metalfx_denoised_stream_desc const& b)
{
    return a.color_format == b.color_format && a.albedo_format == b.albedo_format
        && a.specular_albedo_format == b.specular_albedo_format && a.normal_format == b.normal_format
        && a.roughness_format == b.roughness_format && a.output_format == b.output_format;
}
} // namespace

metalfx_rr_options metalfx_rr_routine::options_for(reconstruct_settings const& settings)
{
    return {.exposure = settings.exposure};
}

bool metalfx_rr_routine::is_available(sg::context const& ctx)
{
    if (!impl::metalfx_denoised_available(ctx))
        return false;
    auto const asset = sgl_shaders::metalfx_prepare.main_cs.asset;
    return asset != nullptr && asset->can_acquire(ctx);
}

cc::shared_async<cc::unit> metalfx_rr_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();
    if (!impl::metalfx_denoised_available(ctx))
    {
        fail_init();
        co_return;
    }
    _passes = {};
    if (!co_await impl::metalfx_build_passes(ctx, _passes))
        fail_init();
    co_return;
}

reconstruct_outcome metalfx_rr_routine::execute(sg::command_list& cmd,
                                                reconstruct_inputs const& in,
                                                reconstruct_history& history,
                                                metalfx_rr_options const& options)
{
    CC_ASSERT(is_set(in.color), "a denoise call needs a colour texture");
    CC_ASSERT(is_set(in.output), "a denoise call needs an output texture");
    CC_ASSERT(in.output.raw() != in.color.raw(), "MetalFX needs an output texture other than its input");

    auto const outcome = [](reconstruct_status status, bool restarted = false)
    { return reconstruct_outcome{.status = status, .denoiser = denoise_method::metalfx_rr, .restarted = restarted}; };

    auto& ctx = cmd.context();
    if (!impl::metalfx_denoised_available(ctx))
        return outcome(reconstruct_status::unsupported);

    // Checked here as well as in the front, because a member is callable directly.
    if (!required_guides(denoise_method::metalfx_rr).without(in.present_guides()).is_empty())
        return outcome(reconstruct_status::unsupported);

    auto const input_extent = extent_of(in.color);
    auto const output_extent = extent_of(in.output);
    CC_ASSERT(output_extent[0] >= input_extent[0] && output_extent[1] >= input_extent[1],
              "MetalFX upscales: the output must be at least the input's extent");
    auto const max_scale = impl::metalfx_denoised_max_scale(ctx);
    if (f32(output_extent[0]) > f32(input_extent[0]) * max_scale
        || f32(output_extent[1]) > f32(input_extent[1]) * max_scale)
        return outcome(reconstruct_status::unsupported);

    auto const self = try_acquire(cmd);
    if (self.is_pending())
        return outcome(reconstruct_status::pending);
    if (self.is_failed())
        return outcome(reconstruct_status::failed);

    auto const staging = impl::metalfx_needs_staging(in.output);
    auto const* const copy = staging ? self->_passes.copy_to(in.output.raw()->format()) : nullptr;
    if (staging && copy == nullptr)
    {
        CC_LOG_ERROR("metalfx: an output of pixel_format {} needs render_target usage", int(in.output.raw()->format()));
        return outcome(reconstruct_status::failed);
    }

    auto restarted = history._prepare(denoise_method::metalfx_rr, input_extent, output_extent);

    // The scaler is created for every texture's format as well as the extents, so another format is another stream.
    auto const desc = impl::metalfx_denoised_stream_desc{
        .input_extent = input_extent,
        .output_extent = output_extent,
        .color_format = in.color.raw()->format(),
        .albedo_format = in.guides.albedo.raw()->format(),
        .specular_albedo_format = in.guides.specular_albedo.raw()->format(),
        .normal_format = in.guides.normal.raw()->format(),
        .roughness_format = in.guides.roughness.raw()->format(),
        .output_format = in.output.raw()->format(),
    };
    auto* state = static_cast<metalfx_rr_state*>(history._member_state.get());
    if (state != nullptr && !same_formats(state->desc, desc))
    {
        history._member_state = nullptr;
        state = nullptr;
    }

    if (state == nullptr)
    {
        auto stream = impl::metalfx_create_denoised_stream(ctx, desc);
        if (stream == nullptr)
            return outcome(reconstruct_status::failed, restarted);

        // The stream defers its own release to an epoch the GPU has passed, and the images are sg's, so the state
        // needs no deleter of its own.
        auto created = std::make_shared<metalfx_rr_state>(metalfx_rr_state{.stream = cc::move(stream), .desc = desc});
        state = created.get();
        history._member_state = cc::move(created);
        history._frame = 0;
        restarted = true;
    }

    impl::metalfx_record_prepare(cmd, self->_passes, state->images, in.guides.depth, in.guides.motion, options.exposure);
    auto const target = impl::metalfx_target(ctx, state->images, in.output);
    if (!impl::metalfx_denoised_dispatch(*state->stream, cmd,
                                         {.color = in.color,
                                          .depth = state->images.depth,
                                          .motion = state->images.motion,
                                          .exposure = state->images.exposure,
                                          .albedo = in.guides.albedo,
                                          .specular_albedo = in.guides.specular_albedo,
                                          .normal = in.guides.normal,
                                          .roughness = in.guides.roughness,
                                          .output = target,
                                          .jitter = in.guides.jitter,
                                          .world_to_view = in.guides.world_to_view,
                                          .view_to_clip = impl::metalfx_view_to_clip(in.guides.view_to_clip),
                                          .reset = restarted}))
        return outcome(reconstruct_status::failed, true);

    if (staging)
        impl::metalfx_record_copy_out(cmd, *copy, state->images, in.output);

    ++history._frame;
    return outcome(reconstruct_status::denoised, restarted);
}
} // namespace sr
