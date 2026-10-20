#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/impl/jitter_sequence.hh>
#include <shaped-rendering/impl/metalfx_backend.hh>
#include <shaped-rendering/metalfx_upscale_routine.hh>
#include <sr_sgl_shaders.hh>
#include <typed-geometry/scalar/scalar.hh>

namespace sr
{
using impl::extent_of;
using impl::is_set;

namespace
{
/// A stream's MetalFX: the scaler and the guides metalfx_prepare.sgl writes for it.
struct metalfx_state
{
    std::shared_ptr<impl::metalfx_stream> stream;
    sg::pixel_format color_format = sg::pixel_format::undefined;
    sg::pixel_format output_format = sg::pixel_format::undefined;
    impl::metalfx_images images;

    /// A dispatch refused this stream since it last encoded, so its next encode resets: MetalFX holds no history yet.
    bool reset_pending = false;
};
} // namespace

f32 metalfx_upscale_routine::ratio_of(render_scale_preset preset)
{
    switch (preset)
    {
    case render_scale_preset::native:
        return 1.0f;
    case render_scale_preset::quality:
        return 1.5f;
    case render_scale_preset::balanced:
        return 1.7f;
    case render_scale_preset::performance:
        return 2.0f;
    }
    return 1.0f;
}

tg::vec2f metalfx_upscale_routine::jitter(u32 frame_index, tg::vec2i input_extent, tg::vec2i output_extent)
{
    return impl::halton_jitter(frame_index, input_extent, output_extent);
}

bool metalfx_upscale_routine::is_available(sg::context const& ctx)
{
    if (!impl::metalfx_available(ctx))
        return false;
    auto const asset = sgl_shaders::metalfx_prepare.main_cs.asset;
    return asset != nullptr && asset->can_acquire(ctx);
}

cc::shared_async<cc::unit> metalfx_upscale_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();
    if (!impl::metalfx_available(ctx))
    {
        fail_init();
        co_return;
    }

    _passes = {};
    if (!co_await impl::metalfx_build_passes(ctx, _passes))
        fail_init();
    co_return;
}

upscale_outcome metalfx_upscale_routine::execute(sg::command_list& cmd, upscale_inputs const& in, upscale_history& history)
{
    CC_ASSERT(is_set(in.color) && is_set(in.output), "an upscale call needs a colour and an output texture");
    CC_ASSERT(is_set(in.depth) && is_set(in.motion), "MetalFX needs the depth and motion guides");
    CC_ASSERT(in.color.raw() != in.output.raw(), "an upscale cannot write the texture it reads");
    CC_ASSERT(extent_of(in.depth) == extent_of(in.color) && extent_of(in.motion) == extent_of(in.color),
              "the guides are at the input extent");

    auto& ctx = cmd.context();
    auto const input_extent = extent_of(in.color);
    auto const output_extent = extent_of(in.output);
    CC_ASSERT(output_extent[0] >= input_extent[0] && output_extent[1] >= input_extent[1],
              "MetalFX upscales: the output must be at least the input's extent");

    if (!impl::metalfx_available(ctx))
        return {.status = reconstruct_status::unsupported};
    auto const max_scale = impl::metalfx_max_scale(ctx);
    if (f32(output_extent[0]) > f32(input_extent[0]) * max_scale
        || f32(output_extent[1]) > f32(input_extent[1]) * max_scale)
        return {.status = reconstruct_status::unsupported};

    auto const self = try_acquire(cmd);
    if (self.is_pending())
        return {.status = reconstruct_status::pending};
    if (self.is_failed())
        return {.status = reconstruct_status::failed};

    // An output MetalFX cannot write is written through an image of our own, copied across after.
    auto const staging = impl::metalfx_needs_staging(in.output);
    auto const* const copy = staging ? self->_passes.copy_to(in.output.raw()->format()) : nullptr;
    // The copy writes the output as an image, so an output MetalFX cannot write needs `image` usage itself.
    if (staging && (copy == nullptr || !in.output.raw()->usage().has(sg::texture_usage::image)))
    {
        CC_LOG_ERROR("metalfx: an output of pixel_format {} needs image usage, and render_target too to skip the copy",
                     int(in.output.raw()->format()));
        return {.status = reconstruct_status::failed};
    }

    auto const restarted = history._prepare(input_extent, output_extent, upscale_method::metalfx);

    // The scaler is created for the two formats as well as the extents, so another format is another stream.
    auto* state = static_cast<metalfx_state*>(history._state.get());
    auto const color_format = in.color.raw()->format();
    auto const output_format = in.output.raw()->format();
    if (state != nullptr && (state->color_format != color_format || state->output_format != output_format))
    {
        history._state = nullptr;
        state = nullptr;
    }

    auto const fresh = state == nullptr;
    if (fresh)
    {
        auto stream = impl::metalfx_create_stream(ctx, {.input_extent = input_extent,
                                                        .output_extent = output_extent,
                                                        .color_format = color_format,
                                                        .output_format = output_format});
        if (stream == nullptr)
            return {.status = reconstruct_status::failed, .restarted = restarted};

        auto created = std::make_shared<metalfx_state>(
            metalfx_state{.stream = cc::move(stream), .color_format = color_format, .output_format = output_format});
        state = created.get();
        history._state = cc::move(created);
    }

    auto const reset = restarted || fresh || state->reset_pending;
    impl::metalfx_record_prepare(cmd, self->_passes, state->images, in.depth, in.motion, in.exposure);
    auto const target = impl::metalfx_target(ctx, state->images, in.output);
    if (!impl::metalfx_dispatch(*state->stream, cmd,
                                {.color = in.color,
                                 .depth = state->images.depth,
                                 .motion = state->images.motion,
                                 .exposure = state->images.exposure,
                                 .output = target,
                                 .jitter = in.jitter,
                                 .reset = reset}))
    {
        state->reset_pending = true;
        return {.status = reconstruct_status::failed, .restarted = true};
    }
    state->reset_pending = false;

    if (staging)
        impl::metalfx_record_copy_out(cmd, *copy, state->images, in.output);

    return {.status = reconstruct_status::denoised, .restarted = reset};
}
} // namespace sr
