#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/denoise_images.hh>
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
/// The near plane metalfx_prepare.sgl divides by; any positive value, since MetalFX only compares depths.
constexpr f32 k_near_plane = 0.1f;

/// The output formats metalfx_copy_out.sgl is compiled for: the HDR ones sr's members write.
constexpr sg::pixel_format k_staged_formats[] = {sg::pixel_format::rgba16_float, sg::pixel_format::rgba32_float};

/// A stream's MetalFX: the scaler and the guides metalfx_prepare.sgl writes for it.
struct metalfx_state
{
    std::shared_ptr<impl::metalfx_stream> stream;
    sg::pixel_format color_format = sg::pixel_format::undefined;
    sg::pixel_format output_format = sg::pixel_format::undefined;
    sg::texture_2d depth;
    sg::texture_2d motion;
    sg::texture_2d exposure;

    /// What MetalFX writes when the caller's output is not renderable; empty otherwise.
    sg::texture_2d staged;
};

/// Element `index` of the Halton sequence in `base`, in [0, 1).
[[nodiscard]] f32 halton(u32 index, u32 base)
{
    auto f = 1.0f;
    auto r = 0.0f;
    for (auto i = index; i > 0; i /= base)
    {
        f /= f32(base);
        r += f * f32(i % base);
    }
    return r;
}
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
    auto const ratio = input_extent[0] > 0 ? f32(output_extent[0]) / f32(input_extent[0]) : 1.0f;
    auto const phases = cc::max(u32(1), u32(tg::ceil(8.0f * ratio * ratio)));
    // Halton's first element is 0 in either base, a sample on the centre; the sequence starts at 1 to skip it.
    auto const i = frame_index % phases + 1;
    return tg::vec2f(halton(i, 2) - 0.5f, halton(i, 3) - 0.5f);
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
    _prepare = nullptr;
    if (!impl::metalfx_available(ctx))
    {
        fail_init();
        co_return;
    }

    auto const prepare = sgl_shaders::metalfx_prepare.main_cs.acquire_pipeline(ctx);
    co_await cc::async_settled(prepare);
    auto const* const pipeline = prepare->try_value();
    if (pipeline == nullptr)
    {
        CC_LOG_ERROR("metalfx: the metalfx_prepare pipeline did not build");
        fail_init();
        co_return;
    }
    _prepare_layout = ctx.cached.acquire_binding_group_layout<sgl_shaders::metalfx_prepare_io>();
    _prepare = *pipeline;

    _copies.clear();
    for (auto const format : k_staged_formats)
    {
        auto const values = sgl_shaders::metalfx_copy_out_options{.output_format = format};
        auto const copy = sgl_shaders::metalfx_copy_out.main_cs.acquire_pipeline(ctx, values);
        co_await cc::async_settled(copy);
        auto const* const copy_pipeline = copy->try_value();
        if (copy_pipeline == nullptr)
        {
            CC_LOG_ERROR("metalfx: the copy out to pixel_format {} did not build", int(format));
            fail_init();
            co_return;
        }
        _copies.push_back({.format = format,
                           .layout = ctx.cached.acquire_binding_group_layout(
                               sgl_shaders::metalfx_copy_out_io::declared_bindings(values)),
                           .pipeline = *copy_pipeline});
    }
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

    // An output MetalFX cannot render to is written through an image of our own, copied across after.
    auto const staging = !in.output.raw()->usage().has(sg::texture_usage::render_target);
    copy_pass const* copy = nullptr;
    if (staging)
    {
        for (auto const& c : self->_copies)
            if (c.format == in.output.raw()->format())
                copy = &c;
        if (copy == nullptr)
        {
            CC_LOG_ERROR("metalfx: an output of pixel_format {} needs render_target usage",
                         int(in.output.raw()->format()));
            return {.status = reconstruct_status::failed};
        }
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
        impl::ensure_image(ctx, created->depth, input_extent, sg::pixel_format::r32_float);
        impl::ensure_image(ctx, created->motion, input_extent, sg::pixel_format::rg16_float);
        impl::ensure_image(ctx, created->exposure, tg::vec2i(1, 1), sg::pixel_format::r16_float);
        state = created.get();
        history._state = cc::move(created);
    }

    auto const group = ctx.transient.create_binding_group(
        cmd, self->_prepare_layout,
        sgl_shaders::metalfx_prepare_io{.linear_depth = in.depth.as_texture_view(),
                                        .motion = in.motion.as_texture_view(),
                                        .device_depth = state->depth.as_image_view<sg::pixel_format::r32_float>(),
                                        .packed_motion = state->motion.as_image_view<sg::pixel_format::rg16_float>(),
                                        .exposure = state->exposure.as_image_view<sg::pixel_format::r16_float>()});
    cmd.compute.bind_pipeline(*self->_prepare);
    cmd.compute.bind_group(0, *group);
    cmd.compute.set_inline_constants(
        sgl_shaders::metalfx_prepare_constants{.near_plane = k_near_plane, .exposure = in.exposure}.to_block());
    cmd.compute.dispatch_threads(input_extent[0], input_extent[1], 1);

    if (staging && (!is_set(state->staged) || extent_of(state->staged) != output_extent))
        state->staged = ctx.persistent.create_texture_2d(
            {.format = in.output.raw()->format(),
             .width = output_extent[0],
             .height = output_extent[1],
             .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::render_target});

    auto const target = staging ? state->staged : in.output;
    if (!impl::metalfx_dispatch(*state->stream, cmd,
                                {.color = in.color,
                                 .depth = state->depth,
                                 .motion = state->motion,
                                 .exposure = state->exposure,
                                 .output = target,
                                 .jitter = in.jitter,
                                 .reset = restarted || fresh}))
        return {.status = reconstruct_status::failed, .restarted = true};

    if (staging)
    {
        auto const copy_group = ctx.transient.create_binding_group(
            cmd, copy->layout,
            sgl_shaders::metalfx_copy_out_io{.source = state->staged.as_texture_view(),
                                             .target = in.output.as_any_image_view()});
        cmd.compute.bind_pipeline(*copy->pipeline);
        cmd.compute.bind_group(0, *copy_group);
        cmd.compute.dispatch_threads(output_extent[0], output_extent[1], 1);
    }

    return {.status = reconstruct_status::denoised, .restarted = restarted || fresh};
}
} // namespace sr
