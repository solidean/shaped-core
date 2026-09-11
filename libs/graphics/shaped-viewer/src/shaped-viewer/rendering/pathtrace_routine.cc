#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/asserts.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh> // cc::async_start
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

/// Where one permutation's compiles stand.
enum class permutation_state
{
    pending, ///< still compiling; the trace that wants it waits a frame
    ready,
    failed, ///< settled without a value, and will until a reload
};

/// One node's state, scheduling it if nobody has yet.
///
/// The cache hands back COLD nodes, so polling alone would watch one that never starts.
/// Started rather than driven: this runs on the frame path, where a compile must not be waited for -- async_start
/// hands it to the ambient scheduler and the trace picks it up a frame or two later.
[[nodiscard]] permutation_state state_of_node(sg::async_compiled_shader const& node)
{
    if (node->try_value() != nullptr)
        return permutation_state::ready;
    if (node->is_ready())
        return permutation_state::failed; // settled with no value
    (void)cc::async_start(node);
    return permutation_state::pending;
}

/// Whether `p` has everything a hit group needs, without waiting for any of it.
[[nodiscard]] permutation_state state_of(material_permutation const* p)
{
    auto const primary = state_of_node(p->shader);
    if (primary != permutation_state::ready)
        return primary;

    if (!p->can_cut_out)
        return permutation_state::ready;

    // The cutout test, twice, because the two rays that reach it carry different payloads.
    auto const any = state_of_node(p->any_hit);
    if (any != permutation_state::ready)
        return any;
    return state_of_node(p->shadow_any_hit);
}

/// The static samplers `hit_groups` declare, by the generated name each register carries.
///
/// The generated text names `sv_sampler_i` at `s{i}` and nothing else records which sampler STATE that is, which is why
/// a permutation carries the states alongside its source.
/// Two permutations claiming one name with the SAME state share it silently; disagreeing about it asserts, because the
/// alternative is an image shaded through the wrong filter with nothing to point at.
/// A per-hit-group local root signature is what resolves the collision, and sg's shader table carries none yet
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

cc::shared_async<cc::unit> pathtrace_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    _raygen_shader = sv::shaders::pathtrace.raygen.PathTraceRayGen->acquire(ctx);
    _miss_shader = sv::shaders::pt_hit.miss.PtMiss->acquire(ctx);
    _shadow_miss_shader = sv::shaders::pt_hit.miss.PtShadowMiss->acquire(ctx);

    // A reload re-acquires the shared shaders, so every pipeline built from the old ones is stale.
    // Cleared before the first await, so nothing built against the previous generation survives the gap.
    _variants.clear();

    // Settled here so `ready` means the three shared stages are in hand.
    // What stays dynamic is the per-material closest-hit set, which only a frame knows -- that is why this routine is
    // fallible even once it is ready.
    co_await cc::async_settled(_raygen_shader);
    co_await cc::async_settled(_miss_shader);
    co_await cc::async_settled(_shadow_miss_shader);
    co_return;
}

pathtrace_routine::pipeline_variant const* pathtrace_routine::_variant_for(sg::context& ctx, pt_trace_desc const& d)
{
    CC_ASSERT(d.bindless != nullptr, "a path trace binds the manager's bindless tables");

    // Started here, and whether or not a substitution ends up needing it.
    //
    // It is a cold node like every other permutation, and one nobody starts is async work that never happens — so the
    // fallback would never become available and every trace missing a real permutation would decline forever.
    // Before the early-out below for the same reason.
    auto const* const fallback
        = d.fallback != nullptr && state_of(d.fallback) == permutation_state::ready ? d.fallback : nullptr;

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
        if (state_of(p) != permutation_state::ready)
            p = fallback; // still compiling, or a material that does not build

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

    if (auto* const resident = _variants.get_ptr(key); resident != nullptr)
    {
        if (resident->pipeline != nullptr)
            return resident;
        if (resident->failed)
            return nullptr;

        // Still building, and polled rather than waited on.
        // A state object discovered on the frame path is exactly the build that must not stall one, so the frames
        // until it lands trace without it.
        if (!resident->pending->is_ready())
            return nullptr;

        auto const* const built = resident->pending->try_value();
        if (built == nullptr)
        {
            resident->failed = true; // remembered, since the same inputs fail the same way until a reload
            resident->pending = {};
            return nullptr;
        }
        resident->pipeline = *built;
        resident->pending = {};
        _finish_variant(ctx, *resident);
        return resident;
    }

    // state_of established every one of these is ready, so the compiled shaders are simply read out here.
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

    // STARTED, never waited on.
    // This is the frame path: a state object is discovered here the first time a material combination is used, so the
    // variant is registered as pending and the trace that wanted it declines until the build lands, which a later
    // frame picks up above.
    variant.pending = ctx.cached.acquire_raytracing_pipeline(rpd);
    variant.pending_raygen = raygen_h;
    variant.pending_miss = miss_h;
    variant.pending_shadow_miss = shadow_miss_h;
    variant.pending_hits = cc::move(hit_handles);

    (void)_variants.entry(key).get_or_emplace(cc::move(variant));
    return nullptr;
}

void pathtrace_routine::_finish_variant(sg::context& ctx, pipeline_variant& variant)
{
    // Miss records in table order: index 0 = primary/bounce miss, index 1 = shadow miss (the raygen's shadow TraceRay passes MissShaderIndex 1).
    auto stbd = sg::raytracing_shader_table_description{.pipeline = variant.pipeline};
    variant.raygen = stbd.add_raygen_shader(variant.pending_raygen);
    (void)stbd.add_miss_shader(variant.pending_miss);
    (void)stbd.add_miss_shader(variant.pending_shadow_miss);
    // In the order built above, so a permutation's primary record sits at the `hit_group_offset` the instances carry and
    // its shadow record at the next index.
    for (auto const h : variant.pending_hits)
        (void)stbd.add_hit_shader(h);
    variant.table = ctx.uncached.create_raytracing_shader_table(stbd);
    variant.pending_hits = {};
}

sg::routine_outcome pathtrace_routine::execute(sg::command_list& cmd, pt_trace_desc const& d)
{
    CC_RECORD_SCOPE("sv.pathtrace");

    // Exclusive, not the read-only scope: _variant_for writes the permutation map.
    auto self = try_acquire_exclusive(cmd);
    if (!self.is_ready())
        return sg::routine_outcome::declined;
    auto& ctx = cmd.context();

    CC_ASSERT(d.output.raw() != nullptr, "pathtrace_routine: no output target bound");

    // The raygen reads the target back to blend into it, and a half float would stop moving the mean long before
    // the estimate is done converging.
    CC_ASSERT(d.output.raw()->description().format == sg::pixel_format::rgba32_float,
              "pathtrace_routine: the accumulator must be rgba32_float — the blend weight is 1 / (accum_frame + 1)");
    CC_ASSERT(d.instance_table.raw() != nullptr, "pathtrace_routine: no instance table bound");
    CC_ASSERT(!d.hit_groups.empty(), "pathtrace_routine: a trace needs at least one hit group to shade with");

    auto const* const variant = self->_variant_for(ctx, d);
    if (variant == nullptr)
        return sg::routine_outcome::declined; // still building, or it failed; leave the target untouched

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
    return sg::routine_outcome::executed;
}
} // namespace sv
