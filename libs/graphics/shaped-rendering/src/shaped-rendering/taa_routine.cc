#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/taa_routine.hh>
#include <sr_sgl_shaders.hh>

namespace sr
{
cc::shared_async<cc::unit> taa_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    auto const vs = sgl_shaders::taa::taa_vs->acquire(ctx);
    auto const ps = sgl_shaders::taa::taa_ps->acquire(ctx);
    co_await cc::async_settled(vs);
    co_await cc::async_settled(ps);

    _group_layout = nullptr;
    _pipeline = {};
    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();
    if (compiled_vs == nullptr || compiled_ps == nullptr)
    {
        fail_init();
        co_return;
    }

    _group_layout = ctx.cached.acquire_binding_group_layout<sgl_shaders::taa::taa_source>();
    _pipeline = ctx.cached.acquire_raster_pipeline(sg::raster_pipeline_description{
        .layout = sgl_shaders::taa::taa_ps.acquire_layout(ctx),
        .vertex_shader = *compiled_vs,
        .fragment_shader = *compiled_ps,
        .topology = sg::primitive_topology::triangle_list, // no vertex input: the vertex index places the triangle
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = sgl_shaders::taa::taa_target::states{.color = {.format = params()},
                                                              .history = {.format = sg::pixel_format::rgba16_float}},
    });
    co_await cc::async_settled(_pipeline);
    co_return;
}

bool taa_routine::is_ready(sg::context& ctx, sg::pixel_format format)
{
    auto const self = try_acquire(ctx, format);
    if (!self.is_ready())
        return false;
    auto const* const pipeline = self->_pipeline != nullptr ? self->_pipeline->try_value() : nullptr;
    return pipeline != nullptr && *pipeline != nullptr;
}

sg::routine_outcome taa_routine::execute(sg::command_list& cmd, taa_desc const& d)
{
    CC_ASSERT(d.current.raw() != nullptr && d.motion.raw() != nullptr && d.depth.raw() != nullptr,
              "taa_routine: a frame, its motion and its depth are all needed");
    CC_ASSERT(d.output.raw() != nullptr && d.history_write.raw() != nullptr, "taa_routine: nowhere to write");
    CC_ASSERT(d.history_read.raw() != nullptr && d.history_write.raw() != nullptr
                  && d.history_write.raw() != d.history_read.raw(),
              "taa_routine: the history is read and written apart, two textures");

    auto const self = try_acquire(cmd, d.output.raw()->description().format);
    if (!self.is_ready())
        return sg::routine_outcome::declined;
    auto const* const pipeline = self->_pipeline != nullptr ? self->_pipeline->try_value() : nullptr;
    if (pipeline == nullptr || *pipeline == nullptr)
        return sg::routine_outcome::declined;

    // Without a history the pass still writes one, from this frame alone; the texture bound to read is then not read.
    auto const valid = d.history_valid;
    auto const group = cmd.context().transient.create_binding_group(
        cmd, self->_group_layout,
        sgl_shaders::taa::taa_source{
            .current = d.current.as_texture_view(),
            .motion = d.motion.as_texture_view(),
            .depth = d.depth.as_texture_view(),
            .history = d.history_read.as_texture_view(),
            .settings = tg::vec4f(d.feedback, valid ? 1.0f : 0.0f, d.clip_width, 0.0f),
        });

    auto pass = cmd.raster.render_to(sgl_shaders::taa::taa_target{
        .color = d.output.as_render_target_view().discarded(),
        .history = d.history_write.as_render_target_view().discarded(),
    });
    pass.bind_pipeline(**pipeline);
    pass.bind_group(0, *group);
    pass.draw({.vertex_range = {.offset = 0, .size = 3}});
    return sg::routine_outcome::executed;
}
} // namespace sr
