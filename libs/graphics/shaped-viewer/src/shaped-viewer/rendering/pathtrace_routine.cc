#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sgl_modules/slug.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-shader-library/raytracing_pipeline.hh>
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

/// Where one permutation's hit group stands.
enum class group_state
{
    pending, ///< still compiling; the trace that wants it waits a frame
    ready,
    failed, ///< settled without a value, and will until a reload
};

/// The state of `p`'s hit group, starting its compile if nobody has yet.
/// The cache hands the node back cold, so polling alone would watch one that never starts.
/// Started on the context's backlog, since nothing awaits it: `sv::background_work` is what a caller ending a run awaits.
/// A compile that failed is logged once, since the stand-in that replaces it says nothing of why.
[[nodiscard]] group_state state_of(sg::context& ctx, material_permutation const* p)
{
    auto const& node = p->hit_group;
    if (!node.is_valid())
        return group_state::failed;
    if (node->try_value() != nullptr)
        return group_state::ready;
    if (node->is_ready())
    {
        if (!p->is_failure_reported)
            CC_LOG_ERROR("the hit group of {} did not compile, so its geometry shades as the stand-in: {}", p->label,
                         node->try_error()->underlying().to_string());
        p->is_failure_reported = true;
        return group_state::failed;
    }
    (void)ctx.backlog.start(node);
    return group_state::pending;
}

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
        = d.fallback != nullptr && state_of(ctx, d.fallback) == group_state::ready ? d.fallback : nullptr;
    auto const* const quadric_fallback
        = d.quadric_fallback != nullptr && state_of(ctx, d.quadric_fallback) == group_state::ready ? d.quadric_fallback
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
        auto const state = state_of(ctx, p);
        if (state != group_state::ready)
        {
            p = p->kind == geometry_kind::quadrics ? quadric_fallback : fallback;
            substituted = true;
            pending = pending || state == group_state::pending;
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
    record.shade_group = instance.hit_group_offset / records_per_permutation;
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

pt_light_table pt_light_table::grouped(cc::span<shaders::tracer::light_record const> lights)
{
    auto out = pt_light_table{};
    for (auto const& l : lights)
    {
        CC_ASSERT(l.path < 4, "a light record names a path the frame block has no slot for");
        ++out.path_count[l.path];
    }

    for (auto i = 1; i < 4; ++i)
        out.path_offset[i] = out.path_offset[i - 1] + out.path_count[i - 1];

    // A counting sort: each light lands at its path's next free slot, so each run keeps the order it was given in.
    out.records = cc::vector<shaders::tracer::light_record>::create_defaulted(lights.size());
    auto next = cc::fixed_array<u32, 4>{out.path_offset[0], out.path_offset[1], out.path_offset[2], out.path_offset[3]};
    for (auto const& l : lights)
        out.records[next[l.path]++] = l;
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
