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
/// Two permutations claiming one name with the SAME state share it silently; disagreeing about it asserts, because the
/// alternative is an image shaded through the wrong filter with nothing to point at.
/// A per-hit-group local root signature is what resolves the collision, and sg's shader table carries none yet
/// (libs/graphics/shaped-viewer/docs/TODO.md).
/// Whether `p` has everything a hit group needs, driving its compiles to completion to find out.
///
/// The cache hands back cold nodes and no async pool is guaranteed here, so they are driven inline exactly as
/// `init_declare` drives the shared shaders — without this a build with no pool finds every permutation cold and
/// traces nothing, forever.
[[nodiscard]] bool is_usable(material_permutation const* p)
{
    (void)cc::try_async_blocking_get(p->shader);
    if (p->shader->try_value() == nullptr)
        return false;

    if (!p->can_cut_out)
        return true;

    // The cutout test, twice, because the two rays that reach it carry different payloads.
    (void)cc::try_async_blocking_get(p->any_hit);
    (void)cc::try_async_blocking_get(p->shadow_any_hit);
    return p->any_hit->try_value() != nullptr && p->shadow_any_hit->try_value() != nullptr;
}

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
            {
                if (s.name != name)
                    continue;
                CC_ASSERT(s.sampler == p->samplers[i],
                          "two materials in one scene claim the same sampler register with "
                          "different states — a per-hit-group local root signature is what "
                          "would give each its own (libs/graphics/shaped-viewer/docs/TODO.md)");
                taken = true;
            }
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

    // Driven here, and whether or not a substitution ends up needing it.
    //
    // It is a cold node like every other permutation, and one nobody drives is async work still outstanding when the
    // frame ends — which is a leak the caller cannot see, since it never asked for this compile in the first place.
    // Before the early-out below for the same reason: a trace that no-ops on its shared shaders must not leave it cold.
    auto const* const fallback = d.fallback != nullptr && is_usable(d.fallback) ? d.fallback : nullptr;

    auto const* const compiled_rg = _raygen_shader->try_value();
    auto const* const compiled_ms = _miss_shader->try_value();
    auto const* const compiled_sms = _shadow_miss_shader->try_value();
    if (compiled_rg == nullptr || compiled_ms == nullptr || compiled_sms == nullptr)
        return nullptr; // a broken edit, or a context accepting no format we can produce — execute no-ops

    // Every generated closest-hit is driven to completion first, and whatever did not land is replaced by the neutral
    // fallback — so the SUBSTITUTED set is what the pipeline is keyed on and built from.
    //
    // Keying on the substitution rather than on what the caller asked for is what makes this self-correcting: the frame
    // a real permutation finally compiles, the key changes and a new variant is built with it.
    auto groups = cc::vector<material_permutation const*>();
    groups.reserve(d.hit_groups.size());
    for (auto const* p : d.hit_groups)
    {
        CC_ASSERT(p != nullptr, "a path trace names a permutation the shader cache does not hold");
        if (!is_usable(p))
            p = fallback; // still in flight, or a material that does not build

        if (p == nullptr)
            return nullptr; // nothing compiled and nothing to stand in for it — trace no-ops, as it always did
        groups.push_back(p);
    }

    // The hit groups in order plus the schema the second group is bound through: the two things a pipeline is built
    // from that a caller can vary between traces.
    auto key_bytes = cc::vector<cc::hash128>();
    key_bytes.reserve(groups.size() + 1);
    key_bytes.push_back(d.bindless->layout()->structural_hash());
    for (auto const* const p : groups)
        key_bytes.push_back(p->key);
    auto const key = cc::hash128::create(cc::span<cc::hash128 const>(key_bytes).as_bytes(), 0);

    if (auto const* const resident = _variants.get_ptr(key); resident != nullptr)
        return resident->pipeline == nullptr ? nullptr : resident;

    // `is_usable` already drove every one of these to completion, so the compiled shaders are simply read out here.
    auto hits = cc::vector<sg::compiled_shader const*>();
    auto any_hits = cc::vector<sg::compiled_shader const*>();
    auto shadow_any_hits = cc::vector<sg::compiled_shader const*>();
    hits.reserve(groups.size());
    any_hits.reserve(groups.size());
    shadow_any_hits.reserve(groups.size());
    for (auto const* const p : groups)
    {
        hits.push_back(p->shader->try_value());

        // The cutout test, where the material has one — twice, because the two rays that reach it carry different payloads.
        any_hits.push_back(p->can_cut_out ? p->any_hit->try_value() : nullptr);
        shadow_any_hits.push_back(p->can_cut_out ? p->shadow_any_hit->try_value() : nullptr);
    }

    // The global root signature must cover every binding *any* stage uses, minus the manager's tables — those are the
    // second group, and merging them here would claim their descriptors twice.
    auto stages = cc::vector<cc::span<sg::binding const>>();
    stages.push_back(compiled_rg->bindings);
    stages.push_back(compiled_ms->bindings);
    stages.push_back(compiled_sms->bindings);
    for (auto const* const h : hits)
        stages.push_back(h->bindings);
    for (auto const* const h : any_hits)
        if (h != nullptr)
            stages.push_back(h->bindings);
    for (auto const* const h : shadow_any_hits)
        if (h != nullptr)
            stages.push_back(h->bindings);

    auto merged = sg::merge_bindings(stages);
    auto own = cc::vector<sg::binding>();
    for (auto& b : merged)
        if (!is_bindless_table(b))
            own.push_back(cc::move(b));

    auto const samplers = collect_samplers(groups);

    auto variant = pipeline_variant{};
    variant.group_layout = ctx.cached.acquire_binding_group_layout(own, samplers);
    // Not a member: the pipeline holds it to keep the root signature alive.
    auto const pipeline_layout
        = ctx.cached.acquire_pipeline_layout({.groups = {variant.group_layout, d.bindless->layout()}});

    // Payload is PtPayload from pt_common.hlsli: rng, the medium (extinction, albedo, g), the wavelength channel, five
    // float3 results, and bsdf_pdf + hit_t = 26 lanes.
    //
    // Depth 2 rather than 1, because the shading moved into the closest-hit: the raygen's trace is the first level and the
    // shadow rays that hit shader casts for next-event estimation are the second.
    auto rpd = sg::raytracing_pipeline_description{.layout = pipeline_layout,
                                                   .max_recursion_depth = 2,
                                                   .max_payload_size = isize(sizeof(u32) * 26)};
    auto const raygen_h = rpd.add_raygen_shader(*compiled_rg);
    auto const miss_h = rpd.add_miss_shader(*compiled_ms);
    auto const shadow_miss_h = rpd.add_miss_shader(*compiled_sms);

    // TWO records per permutation, in the order the instances' `hit_group_offset` indexes them: the primary record at
    // `2 * i`, and the shadow record at `2 * i + 1`.
    //
    // A shadow ray cannot share the primary record.
    // Its any-hit would be invoked carrying a `ShadowPayload` against a declaration of `PtPayload`, and an any-hit declares
    // exactly one payload type — so the two rays need one record each, and `pt_occluded` selects the second with
    // `RayContributionToHitGroupIndex` 1.
    // The shadow record carries no closest hit: the trace skips it.
    auto hit_handles = cc::vector<sg::hit_shader_handle>();
    hit_handles.reserve(hits.size() * 2);
    for (auto i = isize(0); i < hits.size(); ++i)
    {
        auto group = sg::hit_shader{.closest_hit = *hits[i]};
        if (any_hits[i] != nullptr)
            group.any_hit = *any_hits[i];
        hit_handles.push_back(rpd.add_hit_shader(group));

        auto shadow = sg::hit_shader{};
        if (shadow_any_hits[i] != nullptr)
            shadow.any_hit = *shadow_any_hits[i];
        hit_handles.push_back(rpd.add_hit_shader(shadow));
    }

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
    // In the order built above, so a permutation's primary record sits at the `hit_group_offset` the instances carry and
    // its shadow record at the next index.
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

    CC_ASSERT(d.output.raw() != nullptr, "pathtrace_routine: no output target bound");

    // The raygen reads the target back to blend into it, and a half float would stop moving the mean long before
    // the estimate is done converging.
    CC_ASSERT(d.output.raw()->description().format == sg::pixel_format::rgba32_float,
              "pathtrace_routine: the accumulator must be rgba32_float — the blend weight is 1 / (accum_frame + 1)");
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
