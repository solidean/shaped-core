#include <clean-core/record/log.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/blit_routine.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/impl/metalfx_passes.hh>
#include <sr_sgl_shaders.hh>

namespace sr::impl
{
namespace
{
/// The output formats metalfx_copy_out.sgl is compiled for: the HDR ones sr's members write.
constexpr sg::pixel_format k_staged_formats[] = {sg::pixel_format::rgba16_float, sg::pixel_format::rgba32_float};

[[nodiscard]] bool is_staged_format(sg::pixel_format format)
{
    for (auto const f : k_staged_formats)
        if (f == format)
            return true;
    return false;
}
} // namespace

metalfx_passes::copy_pass const* metalfx_passes::copy_to(sg::pixel_format format) const
{
    for (auto const& c : copies)
        if (c.format == format)
            return &c;
    return nullptr;
}

cc::shared_async<bool> metalfx_build_passes(sg::context& ctx, metalfx_passes& out)
{
    out = {};

    // Every pipeline is asked for before any is awaited, so the compiles overlap.
    auto const prepare = sgl_shaders::metalfx_prepare.main_cs.acquire_pipeline(ctx);
    auto copies = cc::vector<decltype(sgl_shaders::metalfx_copy_out.main_cs.acquire_pipeline(ctx))>();
    for (auto const format : k_staged_formats)
        copies.push_back(sgl_shaders::metalfx_copy_out.main_cs.acquire_pipeline(
            ctx, sgl_shaders::metalfx_copy_out_options{.output_format = format}));

    co_await cc::async_settled(prepare);
    auto const* const pipeline = prepare->try_value();
    if (pipeline == nullptr)
    {
        CC_LOG_ERROR("metalfx: the metalfx_prepare pipeline did not build");
        co_return false;
    }
    out.prepare_layout = ctx.cached.acquire_binding_group_layout<sgl_shaders::metalfx_prepare_io>();
    out.prepare = *pipeline;

    for (auto i = isize(0); i < copies.size(); ++i)
    {
        auto const format = k_staged_formats[i];
        co_await cc::async_settled(copies[i]);
        auto const* const copy_pipeline = copies[i]->try_value();
        if (copy_pipeline == nullptr)
        {
            CC_LOG_ERROR("metalfx: the copy out to pixel_format {} did not build", int(format));
            co_return false;
        }
        auto const values = sgl_shaders::metalfx_copy_out_options{.output_format = format};
        out.copies.push_back({.format = format,
                              .layout = ctx.cached.acquire_binding_group_layout(
                                  sgl_shaders::metalfx_copy_out_io::declared_bindings(values)),
                              .pipeline = *copy_pipeline});
    }
    co_return true;
}

metalfx_output_plan metalfx_output_plan_of(metalfx_passes const& passes, sg::texture_2d const& output)
{
    // MetalFX's output usage is ShaderRead | ShaderWrite | RenderTarget, which sg spells `image` plus `render_target`.
    auto const usage = output.raw()->usage();
    auto const format = output.raw()->format();
    auto const has_image = usage.has(sg::texture_usage::image);
    auto const has_render_target = usage.has(sg::texture_usage::render_target);
    if (has_image && has_render_target)
        return {.path = metalfx_output_path::direct, .format = format};
    if (has_image && passes.copy_to(format) != nullptr)
        return {.path = metalfx_output_path::copy, .format = format};
    if (has_render_target)
        return {.path = metalfx_output_path::blit,
                .format = is_staged_format(format) ? format : sg::pixel_format::rgba16_float};
    return {};
}

bool metalfx_record_blit_out(sg::command_list& cmd, metalfx_images const& images, sg::texture_2d const& output)
{
    auto pass = cmd.raster.render_to({.color_targets = {output.as_render_target_view().discarded()}});
    return sr::blit_routine::execute(pass, images.staged) == sg::routine_outcome::executed;
}

void metalfx_record_prepare(sg::command_list& cmd,
                            metalfx_passes const& passes,
                            metalfx_images& images,
                            sg::texture_2d const& depth,
                            sg::texture_2d const& motion,
                            f32 exposure)
{
    auto& ctx = cmd.context();
    auto const extent = extent_of(depth);
    ensure_image(ctx, images.depth, extent, sg::pixel_format::r32_float);
    ensure_image(ctx, images.motion, extent, sg::pixel_format::rg16_float);
    ensure_image(ctx, images.exposure, tg::vec2i(1, 1), sg::pixel_format::r16_float);

    auto const group = ctx.transient.create_binding_group(
        cmd, passes.prepare_layout,
        sgl_shaders::metalfx_prepare_io{.linear_depth = depth.as_texture_view(),
                                        .motion = motion.as_texture_view(),
                                        .device_depth = images.depth.as_image_view<sg::pixel_format::r32_float>(),
                                        .packed_motion = images.motion.as_image_view<sg::pixel_format::rg16_float>(),
                                        .exposure = images.exposure.as_image_view<sg::pixel_format::r16_float>()});
    cmd.compute.bind_pipeline(*passes.prepare);
    cmd.compute.bind_group(0, *group);
    cmd.compute.set_inline_constants(
        sgl_shaders::metalfx_prepare_constants{.near_plane = metalfx_near_plane, .exposure = exposure}.to_block());
    cmd.compute.dispatch_threads(extent[0], extent[1], 1);
}

sg::texture_2d metalfx_target(sg::context& ctx,
                              metalfx_images& images,
                              sg::texture_2d const& output,
                              metalfx_output_plan const& plan)
{
    if (plan.path == metalfx_output_path::direct)
        return output;
    auto const extent = extent_of(output);
    auto const format = plan.format;
    if (!is_set(images.staged) || extent_of(images.staged) != extent || images.staged.raw()->format() != format)
        images.staged = ctx.persistent.create_texture_2d(
            {.format = format,
             .width = extent[0],
             .height = extent[1],
             .usage = sg::texture_usage::texture | sg::texture_usage::image | sg::texture_usage::render_target});
    return images.staged;
}

void metalfx_record_copy_out(sg::command_list& cmd,
                             metalfx_passes::copy_pass const& copy,
                             metalfx_images const& images,
                             sg::texture_2d const& output)
{
    auto const group = cmd.context().transient.create_binding_group(
        cmd, copy.layout,
        sgl_shaders::metalfx_copy_out_io{.source = images.staged.as_texture_view(), .target = output.as_any_image_view()});
    auto const extent = extent_of(output);
    cmd.compute.bind_pipeline(*copy.pipeline);
    cmd.compute.bind_group(0, *group);
    cmd.compute.dispatch_threads(extent[0], extent[1], 1);
}

tg::mat4f metalfx_view_to_clip(tg::mat4f const& view_to_clip)
{
    // Row 3 is what becomes w; its z entry is +1 for a view looking down +z and -1 for one looking down -z, and w is
    // then the linear depth either way, so a row 2 of (0, 0, 0, near) gives `near / depth`.
    auto const forward = view_to_clip[2, 3] < 0.0f ? -1.0f : 1.0f;
    auto m = tg::mat4f::zero;
    for (auto c = 0; c < 4; ++c)
    {
        m[c, 0] = view_to_clip[c, 0];
        m[c, 1] = view_to_clip[c, 1];
    }
    m[3, 2] = metalfx_near_plane;
    m[2, 3] = forward;
    return m;
}
} // namespace sr::impl
