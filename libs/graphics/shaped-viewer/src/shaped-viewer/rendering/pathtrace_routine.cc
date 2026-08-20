#include <clean-core/common/asserts.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <sv_shaders.hh>

namespace sv
{
void pathtrace_routine::init_declare(sg::context& ctx)
{
    auto rg = sv::shaders::pathtrace.raygen.PathTraceRayGen->acquire(ctx);
    auto ms = sv::shaders::pt_hit.miss.PtMiss->acquire(ctx);
    auto sms = sv::shaders::pt_hit.miss.PtShadowMiss->acquire(ctx);
    auto ch = sv::shaders::pt_hit.closest_hit.PtClosestHit->acquire(ctx);

    // No async pool is guaranteed here, so drive the compiles inline.
    (void)cc::try_async_blocking_get(rg);
    (void)cc::try_async_blocking_get(ms);
    (void)cc::try_async_blocking_get(sms);
    (void)cc::try_async_blocking_get(ch);

    auto const* const compiled_rg = rg->try_value();
    auto const* const compiled_ms = ms->try_value();
    auto const* const compiled_sms = sms->try_value();
    auto const* const compiled_ch = ch->try_value();

    // A reload rebuilds the layout, so the old pipeline/table are stale — drop them first.
    _pipeline = nullptr;
    _table = nullptr;
    _group_layout = nullptr;

    if (compiled_rg == nullptr || compiled_ms == nullptr || compiled_sms == nullptr || compiled_ch == nullptr)
        return; // a broken edit, or a context accepting no format we can produce — execute no-ops

    // The global root signature must cover every binding *any* stage uses
    // (raygen: scene/Output/FrameConstants; hit: Materials/Vertices/Indices; miss: background; shadow miss: none).
    _group_layout = ctx.cached.acquire_binding_group_layout(sg::merge_bindings(
        {compiled_rg->bindings, compiled_ms->bindings, compiled_sms->bindings, compiled_ch->bindings}));
    // Not a member: the pipeline holds it to keep the root signature alive.
    auto const pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {_group_layout}});

    // Payload is PtPayload from pt_common.hlsli: albedo + emissive + normal + hit_t = 10 floats.
    auto rpd
        = sg::raytracing_pipeline_description{.layout = pipeline_layout, .max_payload_size = isize(sizeof(float) * 10)};
    auto const raygen_h = rpd.add_raygen_shader(*compiled_rg);
    auto const miss_h = rpd.add_miss_shader(*compiled_ms);
    auto const shadow_miss_h = rpd.add_miss_shader(*compiled_sms);
    auto const hit_h = rpd.add_hit_shader({.closest_hit = *compiled_ch});
    // The build is async and no pool is guaranteed here, so drive it inline like the compiles above.
    auto pipeline_r = cc::try_async_blocking_get(ctx.cached.acquire_raytracing_pipeline(rpd));
    if (pipeline_r.has_error())
        return; // the state object did not build — execute no-ops, as for a broken shader
    _pipeline = cc::move(pipeline_r).value();

    // Miss records in table order: index 0 = primary/bounce miss, index 1 = shadow miss (the raygen's shadow TraceRay passes MissShaderIndex 1).
    auto stbd = sg::raytracing_shader_table_description{.pipeline = _pipeline};
    _raygen = stbd.add_raygen_shader(raygen_h);
    (void)stbd.add_miss_shader(miss_h);
    (void)stbd.add_miss_shader(shadow_miss_h);
    (void)stbd.add_hit_shader(hit_h);
    _table = ctx.uncached.create_raytracing_shader_table(stbd);
}

bool pathtrace_routine::is_ready(sg::command_list& cmd)
{
    // Exclusive, not the const acquire: these are exactly what init_declare writes, and the const path is unlocked
    // (see sg::render_routine's threading note), so reading them there can observe an instance mid-initialization.
    auto self = acquire_exclusive(cmd);
    return self->_pipeline != nullptr && self->_table != nullptr;
}

void pathtrace_routine::execute(sg::command_list& cmd, pt_trace_desc const& d)
{
    auto const& self = acquire(cmd);
    auto& ctx = cmd.context();

    if (self._pipeline == nullptr || self._table == nullptr)
        return; // shaders did not compile, or the pipeline did not build; leave the target untouched

    // The raygen writes both unconditionally, so a missing one faults inside the binding group rather than here.
    CC_ASSERT(d.output.raw() != nullptr, "pathtrace_routine: no output target bound");
    CC_ASSERT(d.gbuffer.raw() != nullptr, "pathtrace_routine: no gbuffer bound");
    CC_ASSERT(d.gbuffer.width() == d.output.width() && d.gbuffer.height() == d.output.height(),
              "pathtrace_routine: the gbuffer must match the output's extent — the raygen writes both at its own "
              "pixel");
    CC_ASSERT(d.history_color.raw() != nullptr && d.history_gbuffer.raw() != nullptr,
              "pathtrace_routine: both history textures must be bound, even with has_history false");
    CC_ASSERT(d.history_color.raw() != d.output.raw() && d.history_gbuffer.raw() != d.gbuffer.raw(),
              "pathtrace_routine: history must not alias what this dispatch writes — reprojection reads another pixel");

    // Refit isn't implemented, so the TLAS is rebuilt each frame from this frame's instances.
    auto const tlas = cmd.raytracing.build_tlas(d.instances);

    auto const group = ctx.transient.create_binding_group(
        self._group_layout, {{.name = "scene", .view = tlas->as_view()},
                             {.name = "Output", .view = d.output.as_readwrite_view()},
                             {.name = "GBuffer", .view = d.gbuffer.as_readwrite_view()},
                             {.name = "HistoryColor", .view = d.history_color.as_readonly_view()},
                             {.name = "HistoryGBuffer", .view = d.history_gbuffer.as_readonly_view()},
                             {.name = "FrameConstants", .view = d.frame.as_uniform_buffer()},
                             {.name = "background", .view = d.background.as_uniform_buffer()},
                             {.name = "Materials", .view = d.materials.as_readonly_buffer()},
                             {.name = "Vertices", .view = d.vertices.as_readonly_buffer()},
                             {.name = "Indices", .view = d.indices.as_readonly_buffer()}});

    cmd.raytracing.bind_pipeline(*self._pipeline);
    cmd.raytracing.bind_group(0, *group);
    cmd.raytracing.dispatch_rays(*self._table, self._raygen, d.output.width(), d.output.height());
}
} // namespace sv
