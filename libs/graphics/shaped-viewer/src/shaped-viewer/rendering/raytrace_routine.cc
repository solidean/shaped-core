#include <clean-core/thread/async.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/rendering/raytrace_routine.hh>
#include <sv_shaders.hh>

namespace sv
{
void pbr_raytrace_routine::init_declare(sg::context& ctx)
{
    auto rg = sv::shaders::raygen.raygen.RayGen->acquire(ctx);
    auto ms = sv::shaders::shading.miss.Miss->acquire(ctx);
    auto ch = sv::shaders::shading.closest_hit.ClosestHit->acquire(ctx);

    // No async pool is guaranteed here, so drive the compiles inline.
    (void)cc::try_async_blocking_get_singlethreaded(rg);
    (void)cc::try_async_blocking_get_singlethreaded(ms);
    (void)cc::try_async_blocking_get_singlethreaded(ch);

    auto const* const compiled_rg = rg->try_value();
    auto const* const compiled_ms = ms->try_value();
    auto const* const compiled_ch = ch->try_value();

    // A reload rebuilds the layout, so the old pipeline/table are stale — drop them first.
    _pipeline = nullptr;
    _table = nullptr;
    _group_layout = nullptr;

    if (compiled_rg == nullptr || compiled_ms == nullptr || compiled_ch == nullptr)
        return; // a broken edit, or a context accepting no format we can produce — execute no-ops

    // The global root signature must cover every binding *any* stage uses
    // (raygen: scene/Output/frame; miss: background; hit: frame/Materials/Vertices/Indices).
    _group_layout = ctx.cached.acquire_binding_group_layout(
        sg::merge_bindings({compiled_rg->bindings, compiled_ms->bindings, compiled_ch->bindings}));
    // Not a member: the pipeline holds it to keep the root signature alive.
    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}});

    auto rpd
        = sg::raytracing_pipeline_description{.layout = pipeline_layout, .max_payload_size = isize(sizeof(float) * 4)};
    auto const raygen_h = rpd.add_raygen_shader(*compiled_rg);
    auto const miss_h = rpd.add_miss_shader(*compiled_ms);
    auto const hit_h = rpd.add_hit_shader({.closest_hit = *compiled_ch});
    _pipeline = ctx.uncached.create_raytracing_pipeline(rpd);

    auto stbd = sg::raytracing_shader_table_description{.pipeline = _pipeline};
    _raygen = stbd.add_raygen_shader(raygen_h);
    (void)stbd.add_miss_shader(miss_h);
    (void)stbd.add_hit_shader(hit_h);
    _table = ctx.uncached.create_raytracing_shader_table(stbd);
}

void pbr_raytrace_routine::execute(sg::command_list& cmd, trace_desc const& d)
{
    auto const& self = acquire(cmd);
    auto& ctx = cmd.context();

    if (self._pipeline == nullptr || self._table == nullptr)
        return; // shaders did not compile; leave the target untouched

    // Refit isn't implemented, so the TLAS is rebuilt each frame from this frame's instances.
    auto const tlas = cmd.raytracing.build_tlas(d.instances);

    auto const group = ctx.transient.create_binding_group(
        self._group_layout, {{.name = "scene", .views = {tlas->as_view()}},
                             {.name = "Output", .views = {d.output.as_readwrite_view()}},
                             {.name = "frame", .views = {d.frame.as_uniform_buffer()}},
                             {.name = "background", .views = {d.background.as_uniform_buffer()}},
                             {.name = "Materials", .views = {d.materials.as_readonly_buffer()}},
                             {.name = "Vertices", .views = {d.vertices.as_readonly_buffer()}},
                             {.name = "Indices", .views = {d.indices.as_readonly_buffer()}}});

    cmd.raytracing.bind_pipeline(*self._pipeline);
    cmd.raytracing.bind_group(0, *group);
    cmd.raytracing.dispatch_rays(*self._table, self._raygen, d.size[0], d.size[1]);
}
} // namespace sv
