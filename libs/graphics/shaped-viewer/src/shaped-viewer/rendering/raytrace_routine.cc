#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/rendering/raytrace_routine.hh>
#include <sv_shaders.hh>

namespace sv
{
cc::shared_async<cc::unit> pbr_raytrace_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    auto const rg = sv::shaders::raygen.raygen.RayGen->acquire(ctx);
    auto const ms = sv::shaders::shading.miss.Miss->acquire(ctx);
    auto const ch = sv::shaders::shading.closest_hit.ClosestHit->acquire(ctx);

    // All three are in flight from their acquire, so settling them one after another costs no concurrency.
    co_await cc::async_settled(rg);
    co_await cc::async_settled(ms);
    co_await cc::async_settled(ch);

    auto const* const compiled_rg = rg->try_value();
    auto const* const compiled_ms = ms->try_value();
    auto const* const compiled_ch = ch->try_value();

    // A reload rebuilds the layout, so the old pipeline/table are stale — drop them first.
    _pipeline = nullptr;
    _table = nullptr;
    _group_layout = nullptr;

    if (compiled_rg == nullptr || compiled_ms == nullptr || compiled_ch == nullptr)
    {
        // A shader that was never good, or a context accepting no format we can produce.
        // Reported as FAILED rather than left pending: a caller that cannot tell the two apart waits forever on
        // something that is not coming.
        fail_init();
        co_return;
    }

    // The global root signature must cover every binding *any* stage uses, which is exactly what common.hlsli
    // declares — so there is nothing to merge and no stage to remember to include in the merge.
    _group_layout = ctx.cached.acquire_binding_group_layout<shaders::flat_bindings>();
    // Not a member: the pipeline holds it to keep the root signature alive.
    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}});

    auto rpd
        = sg::raytracing_pipeline_description{.layout = pipeline_layout, .max_payload_size = isize(sizeof(float) * 4)};
    auto const raygen_h = rpd.add_raygen_shader(*compiled_rg);
    auto const miss_h = rpd.add_miss_shader(*compiled_ms);
    auto const hit_h = rpd.add_hit_shader({.closest_hit = *compiled_ch});
    auto const pipeline = ctx.cached.acquire_raytracing_pipeline(rpd);
    co_await cc::async_settled(pipeline);
    auto const* const built = pipeline->try_value();
    if (built == nullptr)
    {
        fail_init(); // the state object did not build — as final as a broken shader, and reported the same way
        co_return;
    }
    _pipeline = *built;

    auto stbd = sg::raytracing_shader_table_description{.pipeline = _pipeline};
    _raygen = stbd.add_raygen_shader(raygen_h);
    (void)stbd.add_miss_shader(miss_h);
    (void)stbd.add_hit_shader(hit_h);
    _table = ctx.uncached.create_raytracing_shader_table(stbd);
    co_return;
}

sg::routine_outcome pbr_raytrace_routine::execute(sg::command_list& cmd, trace_desc const& d)
{
    auto const self = try_acquire(cmd);
    if (!self.is_ready())
        return sg::routine_outcome::declined;
    auto& ctx = cmd.context();

    if (self->_pipeline == nullptr || self->_table == nullptr)
        return sg::routine_outcome::declined; // nothing to trace with; leave the target untouched

    // Refit isn't implemented, so the TLAS is rebuilt each frame from this frame's instances.
    auto const tlas = cmd.raytracing.build_tlas(d.instances);

    auto const group
        = ctx.transient.create_binding_group(shaders::flat_bindings{.scene = tlas->as_view(),
                                                                    .Output = d.output.as_readwrite_view(),
                                                                    .frame = d.frame.as_uniform_buffer(),
                                                                    .background = d.background.as_uniform_buffer(),
                                                                    .Materials = d.materials.as_readonly_buffer(),
                                                                    .Vertices = d.vertices.as_readonly_buffer(),
                                                                    .Indices = d.indices.as_readonly_buffer()});

    cmd.raytracing.bind_pipeline(*self->_pipeline);
    cmd.raytracing.bind<shaders::flat_bindings>(*group);
    cmd.raytracing.dispatch_rays(*self->_table, self->_raygen, d.size[0], d.size[1]);
    return sg::routine_outcome::executed;
}
} // namespace sv
