#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/math/bit.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sgl_modules/slug.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-shader-library/raytracing_pipeline.hh>
#include <shaped-viewer/rendering/impl/emitters.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/resources/material_shader_cache.hh>
#include <sv_shaders.hh>

namespace sv
{
namespace
{
namespace tracer = sv::shaders::tracer;
using path_t = sv::shaders::tracer_pipeline_path_t;

/// The records a caller's `hit_group_offset` counts per permutation: a surface record and a shadow one.
/// The pipeline's ray set has the same two ray types, so a row of its table is as wide.
constexpr u32 records_per_permutation = 2;
static_assert(path_t::ray_count == int(records_per_permutation), "a row of the table is one record per ray type");

/// Whether `t` is absent or at `output`'s extent.
[[nodiscard]] bool matches_extent(sg::texture_2d const& t, sg::texture_2d const& output)
{
    return t.raw() == nullptr || (t.width() == output.width() && t.height() == output.height());
}
} // namespace

cc::shared_async<cc::unit> pathtrace_routine::init_once(sg::routine_init_scope scope)
{
    _stand_ins = impl::traced_stand_ins::create(scope.context());
    co_return;
}

cc::shared_async<cc::unit> pathtrace_routine::init(sg::routine_init_scope scope)
{
    (void)scope;
    // A reload re-describes every pipeline, so each one built from the previous description is stale.
    _variants.clear();
    co_return;
}

pathtrace_routine::pipeline_variant const* pathtrace_routine::_variant_for(sg::context& ctx,
                                                                           pt_trace_desc const& d,
                                                                           stand_in_report& stand_ins)
{
    stand_ins = {};
    // Started whether or not a substitution ends up needing them, and before any early out.
    auto const* const fallback
        = d.fallback != nullptr && impl::hit_group_state(ctx, d.fallback) == impl::permutation_state::ready ? d.fallback
                                                                                                            : nullptr;
    auto const* const quadric_fallback
        = d.quadric_fallback != nullptr && impl::hit_group_state(ctx, d.quadric_fallback) == impl::permutation_state::ready
            ? d.quadric_fallback
            : nullptr;

    // Whatever has not landed is replaced by the stand-in of its kind, and the SUBSTITUTED set is what the pipeline is keyed on.
    // The frame a permutation's group lands, the key changes and a new variant is built with it.
    auto groups = cc::vector<material_permutation const*>();
    groups.reserve(d.hit_groups.size());
    auto substituted = false;
    auto pending = false;
    for (auto const* p : d.hit_groups)
    {
        CC_ASSERT(p != nullptr, "a path trace names a permutation the shader cache does not hold");
        auto const state = impl::hit_group_state(ctx, p);
        if (state != impl::permutation_state::ready)
        {
            p = p->kind == geometry_kind::quadrics ? quadric_fallback : fallback;
            substituted = true;
            pending = pending || state == impl::permutation_state::pending;
        }
        if (p == nullptr)
            return nullptr;
        groups.push_back(p);
    }
    stand_ins = stand_in_report::of(groups, substituted, pending);

    auto key_parts = cc::vector<cc::hash128>();
    key_parts.reserve(groups.size());
    for (auto const* const p : groups)
        key_parts.push_back(p->key);
    auto const key = cc::hash128::create(cc::span<cc::hash128 const>(key_parts).as_bytes(), 0);

    if (auto* const resident = _variants.get_ptr(key); resident != nullptr)
    {
        if (resident->pipeline != nullptr)
            return resident;
        if (resident->failed)
            return nullptr;

        if (resident->pending_description.is_valid())
        {
            if (!resident->pending_description->is_ready())
                return nullptr;
            auto const* const described = resident->pending_description->try_value();
            if (described == nullptr)
            {
                CC_LOG_ERROR("the path tracer's pipeline did not describe: {}",
                             resident->pending_description->try_error()->underlying().to_string());
                resident->failed = true;
                resident->pending_description = {};
                return nullptr;
            }

            // Group 1 is the manager's own layout, since a group fits only the very layout it was created against.
            // That layout is `tracer.bindless`'s, so the pipeline's footprint resolves by the names the shaders were compiled with.
            auto layouts = cc::small_vector<sg::binding_group_layout_handle, sg::max_binding_groups>();
            layouts.push_back(ctx.cached.acquire_binding_group_layout<tracer::traced>());
            layouts.push_back(d.bindless->layout());
            layouts.push_back(ctx.cached.acquire_binding_group_layout<sgl_modules::slug::tables>());
            auto description = *described;
            description.layout = ctx.cached.acquire_pipeline_layout({.groups = cc::move(layouts)});
            resident->pending_description = {};

            // Started, never waited on: the frames until it lands decline.
            resident->pending = ctx.cached.acquire_raytracing_pipeline(description);
            return nullptr;
        }

        if (!resident->pending->is_ready())
            return nullptr;
        auto const* const built = resident->pending->try_value();
        resident->pending = {};
        if (built == nullptr)
        {
            resident->failed = true;
            return nullptr;
        }
        resident->pipeline = *built;

        auto table = path_t::table_description(resident->pipeline, resident->host);
        auto rows = cc::vector<sg::hit_row>();
        rows.reserve(groups.size());
        for (auto i = 0; i < int(groups.size()); ++i)
            rows.push_back(path_t::add_row(table, {.index = path_t::first_host_hit_group.index + i}));
        resident->table = ctx.uncached.create_raytracing_shader_table(table);
        for (auto const& row : rows)
            resident->row_offsets.push_back(u32(resident->table->offset_of(row)));
        return resident;
    }

    // Each group's hit shaders, one per ray type, in the desc's order: the host's rows follow the pipeline's listed ones.
    auto variant = pipeline_variant{};
    for (auto const* const p : groups)
        for (auto const& shader : *p->hit_group->try_value())
            variant.host.hit_groups.push_back(shader);

    // Stated now and started, never waited on: the frames until it lands decline.
    variant.pending_description = sv::shaders::tracer_pipeline.path.description(ctx, variant.host);
    (void)ctx.backlog.start(variant.pending_description);
    (void)_variants.entry(key).get_or_emplace(cc::move(variant));
    return nullptr;
}

void place_instance_record(shaders::tracer::instance_record& record,
                           sg::tlas_instance const& instance,
                           material_opacity const& opacity)
{
    // The TLAS wire layout is already row-major 3x4, translation in column 3 — exactly the record's rows.
    auto const row = [&](int r)
    {
        auto const* const t = &instance.transform[r * 4];
        return tg::vec4f(t[0], t[1], t[2], t[3]);
    };
    record.to_world_x = row(0);
    record.to_world_y = row(1);
    record.to_world_z = row(2);

    record.opacity_kind = u32(opacity.kind);
    record.opacity_offset = opacity.offset;
    record.opacity_uv_offset = opacity.uv_offset;
    record.opacity_frequency = opacity.frequency;
    record.opacity_channel = opacity.channel;
    record.opacity_sampler = opacity.sampler;
    record.opacity_transform = opacity.transform;
    record.opacity_cutoff = opacity.cutoff;

    // A caller's offset counts two records per permutation, so it names the permutation the instance shades with.
    // An odd one would shade the instance through its neighbor's material, reading its parameter block in another layout.
    CC_ASSERT(instance.hit_group_offset % records_per_permutation == 0, "an instance's hit_group_offset counts two "
                                                                        "records per permutation, so it must be even");
    record.shade_group = instance.hit_group_offset / records_per_permutation;

    // Lights nothing until the trace's light table names it, which only the trace knows.
    record.emitter = no_emitter;
}

shaders::tracer::emitter_record emitter_record_of(u32 instance,
                                                  u32 cdf,
                                                  u32 triangle_count,
                                                  u32 block_count,
                                                  material_emission const& emission)
{
    auto const& color = emission.color;
    auto const& scale = emission.scale;
    return {.instance = instance,
            .cdf = cdf,
            .triangle_count = triangle_count,
            .block_count = block_count,
            .color_kind = u32(color.kind),
            .color_offset = color.offset,
            .color_uv_offset = color.uv_offset,
            .color_frequency = color.frequency,
            .color_channels = color.channels[0] | (color.channels[1] << 8) | (color.channels[2] << 16),
            .color_sampler = color.sampler,
            .color_transform = color.transform,
            .scale_kind = u32(scale.kind),
            .scale_offset = scale.offset,
            .scale_uv_offset = scale.uv_offset,
            .scale_frequency = scale.frequency,
            .scale_channel = scale.channels[0],
            .scale_sampler = scale.sampler,
            .scale_transform = scale.transform,
            .pad0 = 0,
            .pad1 = 0};
}

stand_in_report stand_in_report::of(cc::span<material_permutation const* const> bound, bool substituted, bool pending)
{
    if (!substituted)
        return {};
    // Never 0, which says nothing was stood in for.
    auto h = u64(1);
    for (auto const* const p : bound)
        h = cc::combine_hash(h, cc::make_hash(p->key.low, p->key.high));
    return {.hash = h != 0 ? h : 1, .pending = pending};
}

shaders::tracer::frame_constants default_frame_constants()
{
    auto fc = shaders::tracer::frame_constants{};
    fc.samples_per_pixel = 16;
    fc.max_bounces = 5;
    fc.rng_seed = 1;
    return fc;
}

namespace
{
/// The float right after `x`, which must be finite and not negative.
[[nodiscard]] f32 next_float_up(f32 x)
{
    return cc::bit_cast<f32>(cc::bit_cast<u32>(x) + 1u);
}

/// The float right before `x`, which must be finite and positive.
[[nodiscard]] f32 next_float_down(f32 x)
{
    return cc::bit_cast<f32>(cc::bit_cast<u32>(x) - 1u);
}

/// The luminance of a linear color.
[[nodiscard]] f32 luminance_of(tg::vec3f c)
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/// What a light offers next-event estimation, before it is normalized against the others of its kind.
/// A local light's power, its shaping ignored, which only overweighs a narrow spot; a distant light's irradiance, since it has no finite power.
/// Always finite: a weight that is NaN reads as 0, which the floor below lifts, and one that overflows as the largest f32.
[[nodiscard]] f32 selection_weight_of(shaders::tracer::light_record const& l)
{
    auto const weight = [&]() -> f32
    {
        auto const lum = cc::max(luminance_of(l.emission), 0.0f);
        // An emissive instance's area is an estimate of its world area, and it emits from both faces.
        if (l.path == light_path_mesh)
            return lum * l.area * tg::pi<f32> * 2.0f;
        switch (light_path(l.path))
        {
        case light_path::point:
            return lum * 4.0f * tg::pi<f32>;
        case light_path::area:
            return lum * l.area * tg::pi<f32> * ((l.flags & light_flag_two_sided) != 0 ? 2.0f : 1.0f);
        case light_path::distant_point:
            return lum;
        case light_path::distant_disc:
            return lum * 2.0f * tg::pi<f32> * l.one_minus_cos_angular_radius;
        }
        return 0.0f;
    }();
    if (!(weight > 0.0f))
        return 0.0f;
    return cc::min(weight, impl::largest_finite_f32);
}

/// The share of the choice a kind made only of `light_flag_unseen_emission` records takes beside a kind that has light.
/// Above zero, since such a record may emit; small, since nothing says it does.
constexpr auto unseen_kind_share = 1e-3;

/// The share of the choice picked by power; the rest is uniform over the lights with light to offer.
constexpr auto power_selection_share = 0.5;

/// Writes each record's selection probability and the running sum of them, the last of which is 1.
/// Half of it is by power: local and distant lights are normalized apart and share that half and half where both have light, since their weights do not compare.
/// The other half is uniform over every light with light to offer, which bounds what a dim light near a shading point costs.
/// A record whose emission no read saw above zero is no evidence of light, and it takes none of the uniform half.
/// A kind made only of those takes `unseen_kind_share` of the power half beside one that has light.
/// Every weight is floored at a thousandth of its kind's mean, so no light is impossible to pick while its record says it may be.
void weigh_by_power(cc::span<shaders::tracer::light_record> records)
{
    if (records.empty())
        return;
    auto const is_distant = [](shaders::tracer::light_record const& l)
    { return light_path(l.path) == light_path::distant_point || light_path(l.path) == light_path::distant_disc; };
    auto const is_unseen = [](shaders::tracer::light_record const& l)
    { return l.path == light_path_mesh && (l.flags & light_flag_unseen_emission) != 0; };

    auto raw_total = cc::fixed_array<f64, 2>{0.0, 0.0};
    auto seen_total = cc::fixed_array<f64, 2>{0.0, 0.0};
    auto count = cc::fixed_array<i32, 2>{0, 0};
    auto local_unseen = false;
    for (auto const& l : records)
    {
        auto const kind = is_distant(l) ? 1 : 0;
        auto const w = f64(selection_weight_of(l));
        raw_total[kind] += w;
        ++count[kind];
        if (is_unseen(l))
            local_unseen = true;
        else
            seen_total[kind] += w;
    }
    auto min_weight = cc::fixed_array<f64, 2>{1.0, 1.0};
    for (auto k = 0; k < 2; ++k)
        if (count[k] > 0 && raw_total[k] > 0.0)
            min_weight[k] = 1e-3 * raw_total[k] / f64(count[k]);
    auto const weight = [&](shaders::tracer::light_record const& l)
    {
        auto const kind = is_distant(l) ? 1 : 0;
        return cc::max(f64(selection_weight_of(l)), min_weight[kind]);
    };
    auto total = cc::fixed_array<f64, 2>{0.0, 0.0};
    for (auto const& l : records)
        total[is_distant(l) ? 1 : 0] += weight(l);

    // A kind takes half the choice only where it has light to offer and the other kind has too.
    // Local lights made only of stand-ins take a sliver beside a lit sun, and the whole of the choice beside none; a distant light is never one.
    auto const local_lit = seen_total[0] > 0.0;
    auto const distant_lit = seen_total[1] > 0.0;
    auto const unlit_share = count[0] == 0 ? 0.0 : (count[1] == 0 ? 1.0 : 0.5);
    auto const local_share = [&]
    {
        if (local_lit)
            return distant_lit ? 0.5 : 1.0;
        if (distant_lit)
            return local_unseen ? unseen_kind_share : 0.0;
        return local_unseen ? 1.0 : unlit_share;
    }();
    auto const share = cc::fixed_array<f64, 2>{local_share, 1.0 - local_share};

    // Half of the choice is uniform over the lights with light to offer, so a dim light that is the near one somewhere keeps a
    // fair share: power knows nothing of distance, and alone it starved such a light into noise.
    // A dark light and a stand-in for an emission no read saw take none of that half, or a scene of them would spend it on nothing.
    auto const offers_light
        = [&](shaders::tracer::light_record const& l) { return !is_unseen(l) && selection_weight_of(l) > 0.0f; };
    auto const lit_count = [&]
    {
        auto n = 0;
        for (auto const& l : records)
            n += offers_light(l) ? 1 : 0;
        return n;
    }();
    auto const probability = [&](shaders::tracer::light_record const& l)
    {
        auto const kind = is_distant(l) ? 1 : 0;
        auto const by_power = share[kind] * weight(l) / total[kind];
        if (lit_count == 0)
            return by_power;
        auto const uniform = offers_light(l) ? 1.0 / f64(lit_count) : 0.0;
        return power_selection_share * by_power + (1.0 - power_selection_share) * uniform;
    };

    // The search picks a light with the width of its interval between two f32 cdfs, so that width is the pdf it is given.
    // Every interval is kept open by at least one float step, and the last cdf is exactly 1, among any number of lights.
    auto running = 0.0;
    auto previous = 0.0f;
    for (auto& l : records)
    {
        running += probability(l);
        auto const cdf = cc::min(f32(running), 1.0f);
        l.select_cdf = cdf > previous ? cdf : next_float_up(previous);
        previous = l.select_cdf;
    }
    records.back().select_cdf = 1.0f;
    for (auto i = records.size() - 2; i >= 0; --i)
        records[i].select_cdf = cc::min(records[i].select_cdf, next_float_down(records[i + 1].select_cdf));
    previous = 0.0f;
    for (auto& l : records)
    {
        l.select_pdf = l.select_cdf - previous;
        previous = l.select_cdf;
    }
}
} // namespace

pt_light_table pt_light_table::grouped(cc::span<shaders::tracer::light_record const> lights, light_selection selection)
{
    auto out = pt_light_table{};
    for (auto const& l : lights)
    {
        CC_ASSERT(l.path <= light_path_mesh, "a light record names a path the frame block has no slot for");
        ++out.path_count[l.path];
    }

    for (auto i = 1; i < 5; ++i)
        out.path_offset[i] = out.path_offset[i - 1] + out.path_count[i - 1];

    // A counting sort: each light lands at its path's next free slot, so each run keeps the order it was given in.
    // `light_path_mesh` is the last path, so the emissive instances' records follow every analytic one.
    out.records = cc::vector<shaders::tracer::light_record>::create_defaulted(lights.size());
    auto next = cc::fixed_array<u32, 5>{out.path_offset[0], out.path_offset[1], out.path_offset[2], out.path_offset[3],
                                        out.path_offset[4]};
    for (auto const& l : lights)
        out.records[next[l.path]++] = l;

    if (selection == light_selection::power)
    {
        weigh_by_power(out.records);
    }
    else
    {
        // Written whatever the records carried in, since a record copied out of a power-weighted table would keep its own.
        for (auto& l : out.records)
        {
            l.select_pdf = 0.0f;
            l.select_cdf = 0.0f;
        }
    }
    return out;
}

void pt_light_table::describe_in(shaders::tracer::frame_constants& fc) const
{
    fc.light_count = u32(records.size());
    for (auto i = 0; i < 4; ++i)
    {
        fc.path_offset[i] = path_offset[i];
        fc.path_count[i] = path_count[i];
    }
    fc.emitter_offset = path_offset[light_path_mesh];
}

sg::routine_outcome pathtrace_routine::execute(sg::command_list& cmd, pt_trace_desc const& d)
{
    CC_RECORD_SCOPE("sv.pathtrace");

    // Exclusive, not the read-only scope: _variant_for writes the variant map.
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
    CC_ASSERT(d.bindless != nullptr, "pathtrace_routine: a path trace binds the manager's bindless tables");

    auto const has_guides = d.guide_normal.raw() != nullptr;
    CC_ASSERT(has_guides == (d.guide_depth.raw() != nullptr) && has_guides == (d.guide_albedo.raw() != nullptr),
              "pathtrace_routine: the three guides come together or not at all");
    auto const has_specular_guides = d.guide_specular_albedo.raw() != nullptr;
    CC_ASSERT(has_specular_guides == (d.guide_roughness.raw() != nullptr),
              "pathtrace_routine: the specular albedo and roughness guides come together or not at all");
    CC_ASSERT(!has_specular_guides || has_guides, "pathtrace_routine: the specular guides describe the same image as "
                                                  "the other three, so they need them");
    auto const has_temporal = d.frame_output.raw() != nullptr;
    CC_ASSERT(has_temporal == (d.guide_motion.raw() != nullptr), "pathtrace_routine: the frame output and the motion "
                                                                 "guide come together or not at all");
    auto const has_split = d.frame_diffuse.raw() != nullptr;
    CC_ASSERT(has_split == (d.frame_specular.raw() != nullptr) && has_split == (d.guide_hit_distance.raw() != nullptr),
              "pathtrace_routine: the split-signal targets come together or not at all");
    CC_ASSERT(!has_split || has_temporal, "pathtrace_routine: the split signals are this frame's samples, so they need "
                                          "the temporal targets that define them");
    CC_ASSERT(matches_extent(d.guide_normal, d.output) && matches_extent(d.guide_depth, d.output)
                  && matches_extent(d.guide_albedo, d.output) && matches_extent(d.guide_specular_albedo, d.output)
                  && matches_extent(d.guide_roughness, d.output) && matches_extent(d.frame_output, d.output)
                  && matches_extent(d.guide_motion, d.output) && matches_extent(d.frame_diffuse, d.output)
                  && matches_extent(d.frame_specular, d.output) && matches_extent(d.guide_hit_distance, d.output)
                  && matches_extent(d.primary_depth, d.output),
              "pathtrace_routine: every target the trace writes beside the accumulator matches its extent");
    CC_ASSERT(!d.hit_groups.empty(), "pathtrace_routine: a trace needs at least one hit group to shade with");

    auto stand_ins = stand_in_report{};
    auto const* const variant = self->_variant_for(ctx, d, stand_ins);
    if (variant == nullptr)
        return sg::routine_outcome::declined;
    if (d.stand_ins != nullptr)
        *d.stand_ins = stand_ins;

    // A caller's offset counts two records per permutation, so it names the permutation, whose row it becomes.
    auto instances = cc::vector<sg::tlas_instance>::create_copy_of(d.instances);
    for (auto& instance : instances)
    {
        auto const group = instance.hit_group_offset / records_per_permutation;
        CC_ASSERT(instance.hit_group_offset % records_per_permutation == 0 && group < u32(variant->row_offsets.size()),
                  "pathtrace_routine: an instance's hit_group_offset names a permutation of d.hit_groups, at twice its "
                  "index");
        instance.hit_group_offset = variant->row_offsets[group];
    }
    auto const tlas = cmd.raytracing.build_tlas(instances);

    // A binding cannot be empty, so a trace with no lights binds one zeroed record that `light_count == 0` never reads.
    auto lights = d.lights;
    if (lights.raw() == nullptr)
    {
        lights = ctx.transient.create_buffer<shaders::tracer::light_record>(
            1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        cmd.upload.pod_to_buffer(lights, shaders::tracer::light_record{});
    }

    auto const group = ctx.transient.create_binding_group(
        cmd, ctx.cached.acquire_binding_group_layout<tracer::traced>(),
        impl::traced_group_of(d, self->_stand_ins, tlas, lights, d.frame, self->_stand_ins.paths));
    auto const tables = impl::decal_tables_of(cmd, d, self->_stand_ins);

    cmd.raytracing.bind_pipeline(*variant->pipeline);
    cmd.raytracing.bind_group(0, *group);
    cmd.raytracing.bind_group(bindless_group, *d.bindless->group());
    cmd.raytracing.bind_group(2, *tables);

    // An array the code indexes and nobody declared is logged and barriered whole, so every table is declared.
    // Held across the one trace it is for.
    auto const declared = d.bindless->declare_raytracing_access(cmd);

    cmd.raytracing.dispatch_rays(*variant->table, sg::raygen_index(0), d.output.width(), d.output.height());
    return sg::routine_outcome::executed;
}
} // namespace sv

sv::impl::permutation_state sv::impl::hit_group_state(sg::context& ctx, material_permutation const* p)
{
    auto const& node = p->hit_group;
    if (!node.is_valid())
        return permutation_state::failed;
    // Readiness first: the compile settles on a worker, and a node that held no value a moment ago may hold one now.
    // Once ready it never changes, so it holds a value or an error.
    if (!node->is_ready())
    {
        (void)ctx.backlog.start(node);
        return permutation_state::pending;
    }
    if (node->try_value() != nullptr)
        return permutation_state::ready;
    if (!p->is_failure_reported)
        CC_LOG_ERROR("the hit group of {} did not compile, so its geometry shades as the stand-in: {}", p->label,
                     node->try_error()->underlying().to_string());
    p->is_failure_reported = true;
    return permutation_state::failed;
}
