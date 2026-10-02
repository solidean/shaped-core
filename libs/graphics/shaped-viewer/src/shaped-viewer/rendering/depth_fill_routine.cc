#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/rendering/depth_fill_routine.hh>
#include <sv_shaders.hh>

namespace sv
{
cc::shared_async<cc::unit> depth_fill_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    auto const vs = shaders::depth_fill.main_vs->acquire(ctx);
    auto const ps = shaders::depth_fill.main_ps->acquire(ctx);
    co_await cc::async_settled(vs);
    co_await cc::async_settled(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();

    _group_layout = nullptr;
    _pipeline = nullptr;
    if (compiled_vs == nullptr || compiled_ps == nullptr)
    {
        fail_init();
        co_return;
    }

    _group_layout = ctx.cached.acquire_binding_group_layout<shaders::depth_fill_source>();
    auto const pipeline = ctx.cached.acquire_raster_pipeline({
        .layout = shaders::depth_fill.main_ps.acquire_layout(ctx),
        .vertex_shader = *compiled_vs,
        .fragment_shader = *compiled_ps,
        .topology = sg::primitive_topology::triangle_list, // no vertex input: the vertex index
        .rasterization = {.cull = sg::cull_mode::none},
        .depth_stencil = {.depth_test = true, .depth_write = true, .depth_compare = sg::compare_op::always},
        .depth_stencil_format = params(),
    });
    co_await cc::async_settled(pipeline);
    auto const* const built = pipeline->try_value();
    if (built == nullptr)
    {
        fail_init();
        co_return;
    }
    _pipeline = *built;
    co_return;
}

sg::routine_outcome depth_fill_routine::execute(sg::rendering_scope& scope, sg::texture_2d const& source)
{
    auto& cmd = scope.command_list();
    auto const depth = scope.depth_format();
    CC_ASSERT(depth.has_value(), "a depth fill draws into a scope with a depth target");

    auto self = try_acquire(cmd, depth.value());
    if (!self.is_ready() || self->_pipeline == nullptr)
        return sg::routine_outcome::declined;

    auto& ctx = cmd.context();
    auto const group = ctx.transient.create_binding_group(
        cmd, self->_group_layout, shaders::depth_fill_source{.source = source.as_texture_view()});
    scope.bind_pipeline(*self->_pipeline);
    scope.bind_group(0, *group);
    scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    return sg::routine_outcome::executed;
}
} // namespace sv
