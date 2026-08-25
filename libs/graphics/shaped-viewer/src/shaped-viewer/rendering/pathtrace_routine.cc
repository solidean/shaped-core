#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/asserts.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/resources/material_shader_cache.hh>
#include <sv_shaders.hh>

namespace sv
{
namespace
{
/// Whether `b` is one of the manager's bindless tables rather than a binding of the trace's own.
///
/// A generated closest-hit declares the tables it touches, in sv's spaces, so they come back through reflection like
/// anything else — and they must not: the manager owns that schema, binds it as its own group, and a permutation
/// declaring three of the eight tables would otherwise produce a group layout that is a subset of it.
[[nodiscard]] bool is_bindless_table(sg::binding const& b)
{
    for (auto i = u32(0); i < u32(bindless_table::count_); ++i)
        if (b.name == name_of(bindless_table(i)))
            return true;
    return false;
}

/// The static samplers `hit_groups` declare, by the generated name each register carries.
///
/// The generated text names `sv_sampler_i` at `s{i}` and nothing else records which sampler STATE that is, which is why
/// a permutation carries the states alongside its source.
/// A name already claimed keeps its first state: two permutations disagreeing about `sv_sampler_0` is the register
/// collision a per-permutation local root signature would resolve, and sg's shader table carries none yet
/// (libs/graphics/shaped-viewer/docs/TODO.md).
[[nodiscard]] cc::vector<sg::named_sampler> collect_samplers(cc::span<material_permutation const* const> hit_groups)
{
    auto out = cc::vector<sg::named_sampler>();
    for (auto const* const p : hit_groups)
    {
        for (auto i = isize(0); i < p->samplers.size(); ++i)
        {
            auto name = cc::format("sv_sampler_{}", i);
            auto taken = false;
            for (auto const& s : out)
                taken |= s.name == name;
            if (!taken)
                out.push_back({.name = cc::move(name), .sampler = p->samplers[i]});
        }
    }
    return out;
}
} // namespace

void pathtrace_routine::init_declare(sg::context& ctx)
{
    _raygen_shader = sv::shaders::pathtrace.raygen.PathTraceRayGen->acquire(ctx);
    _miss_shader = sv::shaders::pt_hit.miss.PtMiss->acquire(ctx);
    _shadow_miss_shader = sv::shaders::pt_hit.miss.PtShadowMiss->acquire(ctx);

    // No async pool is guaranteed here, so drive the compiles inline.
    (void)cc::try_async_blocking_get(_raygen_shader);
    (void)cc::try_async_blocking_get(_miss_shader);
    (void)cc::try_async_blocking_get(_shadow_miss_shader);

    // A reload re-acquires the shared shaders, so every pipeline built from the old ones is stale.
    _variants.clear();
    _traced = false;
}

pathtrace_routine::pipeline_variant const* pathtrace_routine::_variant_for(sg::context& ctx, pt_trace_desc const& d)
{
    CC_ASSERT(d.bindless != nullptr, "a path trace binds the manager's bindless tables");

    auto const* const compiled_rg = _raygen_shader->try_value();
    auto const* const compiled_ms = _miss_shader->try_value();
    auto const* const compiled_sms = _shadow_miss_shader->try_value();
    if (compiled_rg == nullptr || compiled_ms == nullptr || compiled_sms == nullptr)
        return nullptr; // a broken edit, or a context accepting no format we can produce — execute no-ops

    // The hit groups in order plus the schema the second group is bound through: the two things a pipeline is built
    // from that a caller can vary between traces.
    auto key_bytes = cc::vector<cc::hash128>();
    key_bytes.reserve(d.hit_groups.size() + 1);
    key_bytes.push_back(d.bindless->layout()->structural_hash());
    for (auto const* const p : d.hit_groups)
    {
        CC_ASSERT(p != nullptr, "a path trace names a permutation the shader cache does not hold");
        key_bytes.push_back(p->key);
    }
    auto const key = cc::hash128::create(cc::span<cc::hash128 const>(key_bytes).as_bytes(), 0);

    if (auto const* const resident = _variants.get_ptr(key); resident != nullptr)
        return resident->pipeline == nullptr ? nullptr : resident;

    // Every generated closest-hit must have landed before a pipeline over the set can be built at all.
    //
    // The cache hands back cold nodes and no async pool is guaranteed here, so they are driven inline exactly as
    // init_declare drives the shared shaders — without this a build with no pool finds every permutation cold and
    // traces nothing, forever.
    auto hits = cc::vector<sg::compiled_shader const*>();
    hits.reserve(d.hit_groups.size());
    for (auto const* const p : d.hit_groups)
    {
        (void)cc::try_async_blocking_get(p->shader);
        auto const* const compiled = p->shader->try_value();
        if (compiled == nullptr)
            return nullptr; // still in flight, or a material that does not compile — retried on a later frame
        hits.push_back(compiled);
    }

    // The global root signature must cover every binding *any* stage uses, minus the manager's tables — those are the
    // second group, and merging them here would claim their descriptors twice.
    auto stages = cc::vector<cc::span<sg::binding const>>();
    stages.push_back(compiled_rg->bindings);
    stages.push_back(compiled_ms->bindings);
    stages.push_back(compiled_sms->bindings);
    for (auto const* const h : hits)
        stages.push_back(h->bindings);

    auto merged = sg::merge_bindings(stages);
    auto own = cc::vector<sg::binding>();
    for (auto& b : merged)
        if (!is_bindless_table(b))
            own.push_back(cc::move(b));

    auto const samplers = collect_samplers(d.hit_groups);

    auto variant = pipeline_variant{};
    variant.group_layout = ctx.cached.acquire_binding_group_layout(own, samplers);
    // Not a member: the pipeline holds it to keep the root signature alive.
    auto const pipeline_layout
        = ctx.cached.acquire_pipeline_layout({.groups = {variant.group_layout, d.bindless->layout()}});

    // Payload is PtPayload from pt_common.hlsli: albedo + emissive + normal + hit_t = 10 floats.
    auto rpd
        = sg::raytracing_pipeline_description{.layout = pipeline_layout, .max_payload_size = isize(sizeof(float) * 10)};
    auto const raygen_h = rpd.add_raygen_shader(*compiled_rg);
    auto const miss_h = rpd.add_miss_shader(*compiled_ms);
    auto const shadow_miss_h = rpd.add_miss_shader(*compiled_sms);

    auto hit_handles = cc::vector<sg::hit_shader_handle>();
    hit_handles.reserve(hits.size());
    for (auto const* const h : hits)
        hit_handles.push_back(rpd.add_hit_shader({.closest_hit = *h}));

    // The build is async and no pool is guaranteed here, so drive it inline like the compiles above.
    auto pipeline_r = cc::try_async_blocking_get(ctx.cached.acquire_raytracing_pipeline(rpd));
    if (pipeline_r.has_error())
    {
        // Remembered as a failure rather than retried every frame: the same shaders would fail the same way, and a
        // reload is what clears it.
        (void)_variants.entry(key).get_or_emplace(pipeline_variant{});
        return nullptr;
    }
    variant.pipeline = cc::move(pipeline_r).value();

    // Miss records in table order: index 0 = primary/bounce miss, index 1 = shadow miss (the raygen's shadow TraceRay passes MissShaderIndex 1).
    auto stbd = sg::raytracing_shader_table_description{.pipeline = variant.pipeline};
    variant.raygen = stbd.add_raygen_shader(raygen_h);
    (void)stbd.add_miss_shader(miss_h);
    (void)stbd.add_miss_shader(shadow_miss_h);
    // In the caller's order, so a hit record's table index is the `hit_group_offset` the instances already carry.
    for (auto const h : hit_handles)
        (void)stbd.add_hit_shader(h);
    variant.table = ctx.uncached.create_raytracing_shader_table(stbd);

    return &_variants.entry(key).get_or_emplace(cc::move(variant));
}

bool pathtrace_routine::is_ready(sg::command_list& cmd)
{
    // Exclusive, not the const acquire: these are exactly what execute writes, and the const path is unlocked
    // (see sg::render_routine's threading note), so reading them there can observe an instance mid-initialization.
    auto self = acquire_exclusive(cmd);
    return self->_traced;
}

void pathtrace_routine::execute(sg::command_list& cmd, pt_trace_desc const& d)
{
    CC_RECORD_SCOPE("sv.pathtrace");

    auto self = acquire_exclusive(cmd);
    auto& ctx = cmd.context();

    self->_traced = false;

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
    CC_ASSERT(d.instance_table.raw() != nullptr, "pathtrace_routine: no instance table bound");
    CC_ASSERT(!d.hit_groups.empty(), "pathtrace_routine: a trace needs at least one hit group to shade with");

    auto const* const variant = self->_variant_for(ctx, d);
    if (variant == nullptr)
        return; // shaders did not compile, or the pipeline did not build; leave the target untouched

    // Refit isn't implemented, so the TLAS is rebuilt each frame from this frame's instances.
    auto const tlas = cmd.raytracing.build_tlas(d.instances);

    auto const group = ctx.transient.create_binding_group(
        variant->group_layout, {{.name = "scene", .view = tlas->as_view()},
                                {.name = "Output", .view = d.output.as_readwrite_view()},
                                {.name = "GBuffer", .view = d.gbuffer.as_readwrite_view()},
                                {.name = "HistoryColor", .view = d.history_color.as_readonly_view()},
                                {.name = "HistoryGBuffer", .view = d.history_gbuffer.as_readonly_view()},
                                {.name = "FrameConstants", .view = d.frame.as_uniform_buffer()},
                                {.name = "background", .view = d.background.as_uniform_buffer()},
                                {.name = "Instances", .view = d.instance_table.as_readonly_buffer()}});

    cmd.raytracing.bind_pipeline(*variant->pipeline);
    cmd.raytracing.bind_group(0, *group);
    cmd.raytracing.bind_group(1, *d.bindless->group());

    // Every bound array binding must be declared before the dispatch, the empty ones included.
    d.bindless->declare_raytracing_access(cmd);

    cmd.raytracing.dispatch_rays(*variant->table, variant->raygen, d.output.width(), d.output.height());
    self->_traced = true;
}
} // namespace sv
