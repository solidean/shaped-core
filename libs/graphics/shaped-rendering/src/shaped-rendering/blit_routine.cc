#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/blit_routine.hh>
#include <sr_shaders.hh>

namespace sr
{
cc::shared_async<cc::unit> blit_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    auto const vs = sr::shaders::blit.vertex.main_vs->acquire(ctx);
    auto const ps = sr::shaders::blit.fragment.main_ps->acquire(ctx);

    // Settled rather than awaited for the value: a shader that did not compile is this routine's verdict to report,
    // not an error to propagate — fail_init says so in the vocabulary a caller already branches on.
    co_await cc::async_settled(vs);
    co_await cc::async_settled(ps);

    auto const* const compiled_vs = vs->try_value();
    auto const* const compiled_ps = ps->try_value();

    // Null only when a shader was never good: a failed reload keeps the last one that compiled and never gets here.
    _group_layout = nullptr;
    _pipeline = {};
    if (compiled_vs == nullptr || compiled_ps == nullptr)
    {
        fail_init(); // not pending: this will not come good until a reload, and a caller should be able to tell
        co_return;
    }

    // The fragment stage carries both bindings: source_texture (t0) and the dynamic linear_sampler (s0).
    _group_layout = ctx.cached.acquire_binding_group_layout(compiled_ps->bindings);

    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}});

    // One instance means one pipeline to build, rather than a map filled lazily on the frame path.
    _pipeline = ctx.cached.acquire_raster_pipeline(sg::raster_pipeline_description{
        .layout = pipeline_layout,
        .vertex_shader = *compiled_vs,
        .fragment_shader = *compiled_ps,
        .topology = sg::primitive_topology::triangle_list, // no vertex input — SV_VertexID
        .rasterization = {.cull = sg::cull_mode::none},
        .color_targets = {{.format = params()}},
    });

    // Awaited HERE rather than polled in execute, which is the whole point of the split: `ready` means ready, so a
    // caller that got the routine handed to it never sees it decline for a few frames while a pipeline finishes.
    co_await cc::async_settled(_pipeline);
    co_return;
}

sg::routine_outcome blit_routine::execute(sg::rendering_scope& scope, sg::texture_2d const& src)
{
    auto& cmd = scope.command_list();
    CC_ASSERT(!scope.color_formats().empty(), "blit must be drawn into a scope with a color target");
    auto const format = scope.color_formats()[0];

    // The format picks the instance, and it is only knowable here — which is what makes this routine fallible.
    auto const self = try_acquire(cmd, format);
    if (!self.is_ready())
        return sg::routine_outcome::declined;

    // Polled rather than waited on.
    // Nothing here may block: execute runs inside the caller's rendering scope, so a wait would stall a frame that has
    // a pass open, and a throw would leave their command list unsubmitted.
    auto const* const pipeline = self->_pipeline != nullptr ? self->_pipeline->try_value() : nullptr;
    if (pipeline == nullptr || *pipeline == nullptr)
        return sg::routine_outcome::declined;

    auto const group = cmd.context().transient.create_binding_group(
        self->_group_layout, {{.name = "source_texture", .view = src.as_readonly_view()}},
        {{.name = "linear_sampler",
          .sampler = {.min_filter = sg::sampler_filter::linear,
                      .mag_filter = sg::sampler_filter::linear,
                      .mip_filter = sg::sampler_filter::nearest,
                      .address_u = sg::sampler_address_mode::clamp_edge,
                      .address_v = sg::sampler_address_mode::clamp_edge}}});

    scope.bind_pipeline(**pipeline);
    scope.bind_group(0, *group);
    scope.draw({.vertex_range = {.offset = 0, .size = 3}});
    return sg::routine_outcome::executed;
}
} // namespace sr
