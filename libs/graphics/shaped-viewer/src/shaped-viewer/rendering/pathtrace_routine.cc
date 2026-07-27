#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <sv_shaders.hh>

namespace sv
{
namespace
{
/// The global root signature must cover every binding *any* stage uses, so merge the four stages' reflected
/// bindings, keyed by name (raygen: scene/Output/FrameConstants; hit: Materials/Vertices; miss: background;
/// shadow miss: none).
void merge_bindings(cc::vector<sg::binding>& into, cc::span<sg::binding const> from)
{
    for (auto const& b : from)
    {
        auto seen = false;
        for (auto const& e : into)
            if (e.name == b.name)
            {
                seen = true;
                break;
            }
        if (!seen)
            into.push_back(b);
    }
}
} // namespace

void pathtrace_routine::init_declare(sg::context& ctx)
{
    auto rg = sv::shaders::pathtrace.raygen.PathTraceRayGen->acquire(ctx);
    auto ms = sv::shaders::pt_hit.miss.PtMiss->acquire(ctx);
    auto sms = sv::shaders::pt_hit.miss.PtShadowMiss->acquire(ctx);
    auto ch = sv::shaders::pt_hit.closest_hit.PtClosestHit->acquire(ctx);

    // No async pool is guaranteed here, so drive the compiles inline.
    (void)cc::try_async_blocking_get_singlethreaded(rg);
    (void)cc::try_async_blocking_get_singlethreaded(ms);
    (void)cc::try_async_blocking_get_singlethreaded(sms);
    (void)cc::try_async_blocking_get_singlethreaded(ch);

    auto const* const compiled_rg = rg->try_value();
    auto const* const compiled_ms = ms->try_value();
    auto const* const compiled_sms = sms->try_value();
    auto const* const compiled_ch = ch->try_value();

    _state.lock(
        [&](state& s)
        {
            // A reload rebuilds the layout, so the old pipeline/table are stale — drop them first.
            s.pipeline = nullptr;
            s.table = nullptr;
            s.pipeline_layout = nullptr;
            s.group_layout = nullptr;

            if (compiled_rg == nullptr || compiled_ms == nullptr || compiled_sms == nullptr || compiled_ch == nullptr)
                return; // a broken edit, or a context accepting no format we can produce — execute no-ops

            auto merged = cc::vector<sg::binding>();
            merge_bindings(merged, compiled_rg->bindings);
            merge_bindings(merged, compiled_ms->bindings);
            merge_bindings(merged, compiled_sms->bindings);
            merge_bindings(merged, compiled_ch->bindings);

            s.group_layout = ctx.cached.acquire_binding_group_layout(merged);
            s.pipeline_layout = ctx.cached.acquire_pipeline_layout({.groups = {s.group_layout}});

            // Payload is PtPayload from pt_common.hlsli: albedo + emissive + normal + hit_t = 10 floats.
            auto rpd = sg::raytracing_pipeline_description{.layout = s.pipeline_layout,
                                                           .max_payload_size = isize(sizeof(float) * 10)};
            auto const raygen_h = rpd.add_raygen_shader(*compiled_rg);
            auto const miss_h = rpd.add_miss_shader(*compiled_ms);
            auto const shadow_miss_h = rpd.add_miss_shader(*compiled_sms);
            auto const hit_h = rpd.add_hit_shader({.closest_hit = *compiled_ch});
            s.pipeline = ctx.uncached.create_raytracing_pipeline(rpd);

            // Miss records in table order: index 0 = primary/bounce miss, index 1 = shadow miss (the raygen's
            // shadow TraceRay passes MissShaderIndex 1).
            auto stbd = sg::raytracing_shader_table_description{.pipeline = s.pipeline};
            s.raygen = stbd.add_raygen_shader(raygen_h);
            (void)stbd.add_miss_shader(miss_h);
            (void)stbd.add_miss_shader(shadow_miss_h);
            (void)stbd.add_hit_shader(hit_h);
            s.table = ctx.uncached.create_raytracing_shader_table(stbd);
        });
}

void pathtrace_routine::execute(sg::command_list& cmd, pt_trace_desc const& d)
{
    auto& self = acquire(cmd);
    auto& ctx = cmd.context();

    self._state.lock(
        [&](state& s)
        {
            if (s.pipeline == nullptr || s.table == nullptr)
                return; // shaders did not compile; leave the target untouched

            // Refit isn't implemented, so the TLAS is rebuilt each frame from this frame's instances.
            auto const tlas = cmd.raytracing.build_tlas(d.instances);

            auto const group = ctx.transient.create_binding_group(
                s.group_layout, {{.name = "scene", .view = tlas->as_view()},
                                 {.name = "Output", .view = d.output.as_readwrite_view()},
                                 {.name = "FrameConstants", .view = d.frame.as_uniform_buffer()},
                                 {.name = "background", .view = d.background.as_uniform_buffer()},
                                 {.name = "Materials", .view = d.materials.as_readonly_buffer()},
                                 {.name = "Vertices", .view = d.vertices.as_readonly_buffer()}});

            cmd.raytracing.bind_pipeline(*s.pipeline);
            cmd.raytracing.bind_group(0, *group);
            cmd.raytracing.dispatch_rays(*s.table, s.raygen, d.output.width(), d.output.height());
        });
}
} // namespace sv
