#include <clean-core/common/assert.hh>
#include <sgl_modules/slug.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/rendering/traced_group.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>

namespace sv
{
namespace
{
namespace tracer = sv::shaders::tracer;

/// `t` as an image view of `Format`, or `stand_in` where the trace writes nothing to it.
template <sg::pixel_format Format>
[[nodiscard]] auto image_or(sg::texture_2d const& t, sg::texture_2d const& stand_in)
{
    return (t.raw() != nullptr ? t : stand_in).as_image_view<Format>();
}

/// The counters a state with `max_permutations` needs: one per queue, the sample floor and its shortfall, then a count and a cursor per permutation.
/// Seven before the permutations', which `tracer.counter_permutations` is the count of.
[[nodiscard]] isize counter_count(i32 max_permutations)
{
    return 7 + 2 * isize(max_permutations);
}
} // namespace

wavefront_state wavefront_state::create(sg::context& ctx, tg::vec2i extent, i32 max_permutations)
{
    CC_ASSERT(extent[0] > 0 && extent[1] > 0, "a wavefront state needs at least one pixel");
    CC_ASSERT(max_permutations > 0, "a wavefront state needs room for at least one permutation");

    auto const slots = isize(extent[0]) * isize(extent[1]);
    auto const usage = sg::buffer_usage::readwrite_buffer | sg::buffer_usage::copy_src;
    auto& p = ctx.persistent;
    return {.extent = extent,
            .max_permutations = max_permutations,
            .path_rays = p.create_buffer<tracer::path_ray>(slots, usage),
            .path_states = p.create_buffer<tracer::path_state>(slots, usage),
            .path_media = p.create_buffer<tracer::path_medium>(slots, usage),
            .path_lights = p.create_buffer<tracer::path_light>(slots, usage),
            .hits = p.create_buffer<tracer::hit_record>(slots, usage),
            .shadow_rays = p.create_buffer<tracer::shadow_ray>(2 * slots, usage),
            .tallies = p.create_buffer<tracer::pixel_tally>(slots, usage),
            .queue_a = p.create_buffer<u32>(slots, usage),
            .queue_b = p.create_buffer<u32>(slots, usage),
            .queue_hits = p.create_buffer<u32>(slots, usage),
            .queue_sorted = p.create_buffer<u32>(slots, usage),
            .queue_shadow = p.create_buffer<u32>(slots, usage),
            .queue_finished = p.create_buffer<u32>(slots, usage),
            .counters = p.create_buffer<u32>(counter_count(max_permutations), usage),
            .shade_offsets = p.create_buffer<u32>(max_permutations, usage),
            // One record per stage counting a queue, then one per permutation: `tracer.dispatch_shade` is the first of those.
            .args = p.create_buffer<tracer::dispatch_args>(4 + isize(max_permutations),
                                                           usage | sg::buffer_usage::indirect_command_buffer)};
}

isize wavefront_state::size_in_bytes() const
{
    return path_rays.size_in_bytes() + path_states.size_in_bytes() + path_media.size_in_bytes()
         + path_lights.size_in_bytes() + hits.size_in_bytes() + shadow_rays.size_in_bytes() + tallies.size_in_bytes()
         + queue_a.size_in_bytes() + queue_b.size_in_bytes() + queue_hits.size_in_bytes() + queue_sorted.size_in_bytes()
         + queue_shadow.size_in_bytes() + queue_finished.size_in_bytes() + counters.size_in_bytes()
         + shade_offsets.size_in_bytes() + args.size_in_bytes();
}

namespace impl
{
traced_stand_ins traced_stand_ins::create(sg::context& ctx)
{
    auto const image = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d(
            {.format = format, .width = 1, .height = 1, .usage = sg::texture_usage::image});
    };
    auto const table = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d(
            {.format = format, .width = 1, .height = 1, .usage = sg::texture_usage::texture});
    };
    auto paths = wavefront_state::create(ctx, tg::vec2i(1, 1), 1);
    return {.guide_normal = image(sg::pixel_format::rgba16_float),
            .guide_depth = image(sg::pixel_format::r32_float),
            .guide_albedo = image(sg::pixel_format::rgba16_float),
            .guide_specular_albedo = image(sg::pixel_format::rgba16_float),
            .guide_roughness = image(sg::pixel_format::r16_float),
            .frame_output = image(sg::pixel_format::rgba16_float),
            .frame_diffuse = image(sg::pixel_format::rgba16_float),
            .frame_specular = image(sg::pixel_format::rgba16_float),
            .guide_hit_distance = image(sg::pixel_format::rg32_float),
            .guide_motion = image(sg::pixel_format::rg32_float),
            .primary_depth = image(sg::pixel_format::r32_float),
            .sample_counts = image(sg::pixel_format::r32_float),
            .decal_curves = table(sg::pixel_format::rgba16_float),
            .decal_bands = table(sg::pixel_format::rg16_uint),
            .decals = ctx.persistent.create_buffer<tracer::decal_record>(1, sg::buffer_usage::readonly_buffer),
            .decal_shapes = ctx.persistent.create_buffer<tracer::decal_shape>(1, sg::buffer_usage::readonly_buffer),
            .emitters = ctx.persistent.create_buffer<tracer::emitter_record>(1, sg::buffer_usage::readonly_buffer),
            .paths = cc::move(paths)};
}

shaders::tracer::traced traced_group_of(pt_trace_desc const& d,
                                        traced_stand_ins const& s,
                                        sg::tlas_handle const& tlas,
                                        sg::buffer<shaders::tracer::light_record> const& lights,
                                        sg::buffer<shaders::tracer::frame_constants> const& frame,
                                        wavefront_state const& paths)
{
    return tracer::traced{
        .world = tlas->as_view(),
        .output = d.output.as_image_view<sg::pixel_format::rgba32_float>(),
        .frame = frame.as_readonly_buffer(),
        .instances = d.instance_table.as_readonly_buffer(),
        .background = d.background.reinterpret_as<tg::vec4f>().as_readonly_buffer(),
        .guide_normal = image_or<sg::pixel_format::rgba16_float>(d.guide_normal, s.guide_normal),
        .guide_depth = image_or<sg::pixel_format::r32_float>(d.guide_depth, s.guide_depth),
        .guide_albedo = image_or<sg::pixel_format::rgba16_float>(d.guide_albedo, s.guide_albedo),
        .guide_specular_albedo
        = image_or<sg::pixel_format::rgba16_float>(d.guide_specular_albedo, s.guide_specular_albedo),
        .guide_roughness = image_or<sg::pixel_format::r16_float>(d.guide_roughness, s.guide_roughness),
        .frame_output = image_or<sg::pixel_format::rgba16_float>(d.frame_output, s.frame_output),
        .guide_motion = image_or<sg::pixel_format::rg32_float>(d.guide_motion, s.guide_motion),
        .frame_diffuse = image_or<sg::pixel_format::rgba16_float>(d.frame_diffuse, s.frame_diffuse),
        .frame_specular = image_or<sg::pixel_format::rgba16_float>(d.frame_specular, s.frame_specular),
        .guide_hit_distance = image_or<sg::pixel_format::rg32_float>(d.guide_hit_distance, s.guide_hit_distance),
        .lights = lights.as_readonly_buffer(),
        .emitters = (d.emitters.raw() != nullptr ? d.emitters : s.emitters).as_readonly_buffer(),
        .primary_depth = image_or<sg::pixel_format::r32_float>(d.primary_depth, s.primary_depth),
        .sample_counts = image_or<sg::pixel_format::r32_float>(d.sample_counts, s.sample_counts),
        .decals = (d.decals.raw() != nullptr ? d.decals : s.decals).as_readonly_buffer(),
        .decal_shapes = (d.decal_shapes.raw() != nullptr ? d.decal_shapes : s.decal_shapes).as_readonly_buffer(),
        .path_rays = paths.path_rays.as_readwrite_buffer(),
        .path_states = paths.path_states.as_readwrite_buffer(),
        .path_media = paths.path_media.as_readwrite_buffer(),
        .path_lights = paths.path_lights.as_readwrite_buffer(),
        .hits = paths.hits.as_readwrite_buffer(),
        .shadow_rays = paths.shadow_rays.as_readwrite_buffer(),
        .tallies = paths.tallies.as_readwrite_buffer(),
        .queue_a = paths.queue_a.as_readwrite_buffer(),
        .queue_b = paths.queue_b.as_readwrite_buffer(),
        .queue_hits = paths.queue_hits.as_readwrite_buffer(),
        .queue_sorted = paths.queue_sorted.as_readwrite_buffer(),
        .queue_shadow = paths.queue_shadow.as_readwrite_buffer(),
        .queue_finished = paths.queue_finished.as_readwrite_buffer(),
        .counters = paths.counters.as_readwrite_buffer(),
        .shade_offsets = paths.shade_offsets.as_readwrite_buffer(),
    };
}

sg::binding_group_handle decal_tables_of(sg::command_list& cmd, pt_trace_desc const& d, traced_stand_ins const& s)
{
    // An atlas nothing was placed in has no textures yet, so the stand-ins serve it as they serve no atlas at all.
    auto const has_atlas = d.decal_atlas != nullptr && d.decal_atlas->curve_texture().raw() != nullptr
                        && d.decal_atlas->band_texture().raw() != nullptr;
    return cmd.context().transient.create_binding_group(
        cmd, cmd.context().cached.acquire_binding_group_layout<sgl_modules::slug::tables>(),
        sgl_modules::slug::tables{
            .curves = (has_atlas ? d.decal_atlas->curve_texture() : s.decal_curves).as_texture_view(),
            .bands = (has_atlas ? d.decal_atlas->band_texture() : s.decal_bands).as_texture_view()});
}
} // namespace impl
} // namespace sv
