#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-library/raytracing_pipeline.hh>
#include <shaped-viewer/rendering/sgl_pathtrace_routine.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/shader_library.hh>
#include <sv_sgl_shaders.hh>

namespace sv
{
namespace
{
namespace tracer = sv::sgl_shaders::tracer;
using path_t = sv::sgl_shaders::tracer_pipeline_path_t;

/// sv's fallback material, `material_shader_cache::acquire_fallback`, as a hit group of the SGL tracer.
///
/// The material is the HLSL one's fragment over OpenPBR's defaults, so the two tracers shade the same surface.
/// The ray set is restated rather than named as `tracer.path_rays`, since a hit group names a set of its own file.
/// slib holds the two to the same ray types and payloads.
constexpr cc::string_view k_fallback_hit_group = R"(require raytracing_pipeline

use material
use openpbr
use tracer

rays path_rays:
    surface: tracer.surface_payload
    occlusion: tracer.shadow_payload

/// What `sv_fallback` writes over the default surface: a neutral gray, fully rough.
fun fallback_surface(ctx: material.shading_context) -> openpbr.surface:
    let mut surface = openpbr.default_surface()
    surface.base_color = float3(0.5, 0.5, 0.5)
    surface.specular_roughness = 1.0
    return surface

@closest_hit fun fallback_hit(h: triangle_hit, p: mut tracer.surface_payload){tracer.traced, tracer.bindless}:
    let ctx = tracer.triangle_context(h)
    tracer.shade_triangle(h, mut p, ctx, fallback_surface(ctx), false)

hit_group fallback for path_rays:
    surface = (closest_hit = fallback_hit)
    occlusion = ()
)";

/// `t` as an image view of `Format`, or `stand_in` where the trace writes nothing to it.
template <sg::pixel_format Format>
[[nodiscard]] auto image_or(sg::texture_2d const& t, sg::texture_2d const& stand_in)
{
    return (t.raw() != nullptr ? t : stand_in).as_image_view<Format>();
}
} // namespace

cc::string_view sgl_pathtrace_routine::fallback_hit_group_source()
{
    return k_fallback_hit_group;
}

cc::shared_async<cc::unit> sgl_pathtrace_routine::init_once(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();
    auto const stand_in = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d(
            {.format = format, .width = 1, .height = 1, .usage = sg::texture_usage::image});
    };
    _guide_normal_stand_in = stand_in(sg::pixel_format::rgba16_float);
    _guide_depth_stand_in = stand_in(sg::pixel_format::r32_float);
    _guide_albedo_stand_in = stand_in(sg::pixel_format::rgba16_float);
    _guide_specular_albedo_stand_in = stand_in(sg::pixel_format::rgba16_float);
    _guide_roughness_stand_in = stand_in(sg::pixel_format::r16_float);
    _frame_output_stand_in = stand_in(sg::pixel_format::rgba16_float);
    _frame_diffuse_stand_in = stand_in(sg::pixel_format::rgba16_float);
    _frame_specular_stand_in = stand_in(sg::pixel_format::rgba16_float);
    _guide_hit_distance_stand_in = stand_in(sg::pixel_format::rg32_float);
    _guide_motion_stand_in = stand_in(sg::pixel_format::rg32_float);
    _primary_depth_stand_in = stand_in(sg::pixel_format::r32_float);
    co_return;
}

cc::shared_async<cc::unit> sgl_pathtrace_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // A reload re-describes the pipeline, so every one built from the previous description is stale.
    _variants.clear();
    _is_described = false;
    _host = {};

    auto const lib = acquire_shader_library();
    if (lib.has_error())
    {
        CC_LOG_ERROR("no shader library to compile the SGL path tracer's hit group through: {}", lib.error().to_string());
        co_return;
    }

    auto const hits = slib::compile_hit_group(&ctx, lib.value(), &path_t::definition(),
                                              cc::string(k_fallback_hit_group), "fallback", "<sv fallback hit group>");
    co_await cc::async_settled(hits);
    if (hits->try_value() == nullptr)
    {
        CC_LOG_ERROR("the SGL path tracer's fallback hit group did not compile: {}",
                     hits->try_error()->underlying().to_string());
        co_return;
    }
    _host.hit_groups = *hits->try_value();

    auto const description = sv::sgl_shaders::tracer_pipeline.path.description(ctx, _host);
    co_await cc::async_settled(description);
    if (description->try_value() == nullptr)
    {
        CC_LOG_ERROR("the SGL path tracer did not describe: {}", description->try_error()->underlying().to_string());
        co_return;
    }
    _description = *description->try_value();
    _is_described = true;
    co_return;
}

sgl_pathtrace_routine::pipeline_variant const* sgl_pathtrace_routine::_variant_for(
    sg::context& ctx,
    sg::binding_group_layout_handle const& bindless_layout)
{
    if (!_is_described)
        return nullptr;

    auto const key = bindless_layout->structural_hash();
    if (auto* const resident = _variants.get_ptr(key); resident != nullptr)
    {
        if (resident->pipeline != nullptr)
            return resident;
        if (resident->failed || !resident->pending->is_ready())
            return nullptr;

        auto const* const built = resident->pending->try_value();
        resident->pending = {};
        if (built == nullptr)
        {
            resident->failed = true;
            return nullptr;
        }
        resident->pipeline = *built;

        auto table = path_t::table_description(resident->pipeline, _host);
        auto const row = path_t::add_row(table, path_t::first_host_hit_group);
        resident->table = ctx.uncached.create_raytracing_shader_table(table);
        resident->hit_group_offset = u32(resident->table->offset_of(row));
        return resident;
    }

    // The manager's bindless group is bound as it is, and a group fits only the very layout it was created against.
    // So group 1 of the pipeline is the manager's layout, which module `tracer`'s `bindless` lays out slot for slot.
    //
    // TEMPORARY: that layout names a table `gBindlessBuffers` where SGL says `bindless.buffers`, and sg resolves a footprint
    // by name, so this pipeline has none and every bound view is barriered by its class.
    // Naming sv's tables as SGL does, once the HLSL tracer retires, is what lets the footprint through.
    auto groups = cc::small_vector<sg::binding_group_layout_handle, sg::max_binding_groups>();
    groups.push_back(ctx.cached.acquire_binding_group_layout<tracer::traced>());
    groups.push_back(bindless_layout);

    auto description = _description;
    description.layout = ctx.cached.acquire_pipeline_layout({.groups = cc::move(groups)});

    // Started, never waited on: the frames until it lands decline.
    auto variant = pipeline_variant{};
    variant.pending = ctx.cached.acquire_raytracing_pipeline(description);
    (void)_variants.entry(key).get_or_emplace(cc::move(variant));
    return nullptr;
}

sg::routine_outcome sgl_pathtrace_routine::execute(sg::command_list& cmd, pt_trace_desc const& d)
{
    CC_RECORD_SCOPE("sv.sgl_pathtrace");

    // Exclusive: _variant_for writes the variant map.
    auto self = try_acquire_exclusive(cmd);
    if (!self.is_ready())
        return sg::routine_outcome::declined;
    auto& ctx = cmd.context();

    CC_ASSERT(d.output.raw() != nullptr, "sgl_pathtrace_routine: no output target bound");
    CC_ASSERT(d.output.raw()->description().format == sg::pixel_format::rgba32_float,
              "sgl_pathtrace_routine: the accumulator must be rgba32_float — the blend weight is 1 / (accum_frame + "
              "1)");
    CC_ASSERT(d.instance_table.raw() != nullptr, "sgl_pathtrace_routine: no instance table bound");
    CC_ASSERT(d.bindless != nullptr, "sgl_pathtrace_routine: a path trace binds the manager's bindless tables");

    // Both or none of each set, as pathtrace_routine asserts at length; the SGL tracer binds what it is given.
    auto const has_guides = d.guide_normal.raw() != nullptr;
    auto const has_specular_guides = d.guide_specular_albedo.raw() != nullptr;
    auto const has_temporal = d.frame_output.raw() != nullptr;
    auto const has_split = d.frame_diffuse.raw() != nullptr;
    CC_ASSERT(has_guides == (d.guide_depth.raw() != nullptr) && has_guides == (d.guide_albedo.raw() != nullptr),
              "sgl_pathtrace_routine: the three guides come together or not at all");
    CC_ASSERT(has_specular_guides == (d.guide_roughness.raw() != nullptr), "sgl_pathtrace_routine: the specular guides "
                                                                           "come together or not at all");
    CC_ASSERT(has_temporal == (d.guide_motion.raw() != nullptr), "sgl_pathtrace_routine: the frame output and the "
                                                                 "motion guide come together or not at all");
    CC_ASSERT(has_split == (d.frame_specular.raw() != nullptr) && has_split == (d.guide_hit_distance.raw() != nullptr),
              "sgl_pathtrace_routine: the split-signal targets come together or not at all");

    auto const* const variant = self->_variant_for(ctx, d.bindless->layout());
    if (variant == nullptr)
        return sg::routine_outcome::declined;

    // Every instance shades with the one host hit group, whatever the caller's offsets index in the HLSL tracer's table.
    auto instances = cc::vector<sg::tlas_instance>::create_copy_of(d.instances);
    for (auto& instance : instances)
        instance.hit_group_offset = variant->hit_group_offset;
    auto const tlas = cmd.raytracing.build_tlas(instances);

    // A binding cannot be empty, so a trace with no lights binds one zeroed record that `light_count == 0` never reads.
    auto lights = d.lights;
    if (lights.raw() == nullptr)
    {
        lights
            = ctx.transient.create_buffer<light_gpu>(1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        cmd.upload.pod_to_buffer(lights, light_gpu{});
    }

    auto const group = ctx.transient.create_binding_group(
        cmd, ctx.cached.acquire_binding_group_layout<tracer::traced>(),
        tracer::traced{
            .world = tlas->as_view(),
            .output = d.output.as_image_view<sg::pixel_format::rgba32_float>(),
            .frame = d.frame.reinterpret_as<tracer::frame_constants>().as_readonly_buffer(),
            .instances = d.instance_table.reinterpret_as<tracer::instance_record>().as_readonly_buffer(),
            .background = d.background.reinterpret_as<tg::vec4f>().as_readonly_buffer(),
            .guide_normal = image_or<sg::pixel_format::rgba16_float>(d.guide_normal, self->_guide_normal_stand_in),
            .guide_depth = image_or<sg::pixel_format::r32_float>(d.guide_depth, self->_guide_depth_stand_in),
            .guide_albedo = image_or<sg::pixel_format::rgba16_float>(d.guide_albedo, self->_guide_albedo_stand_in),
            .guide_specular_albedo
            = image_or<sg::pixel_format::rgba16_float>(d.guide_specular_albedo, self->_guide_specular_albedo_stand_in),
            .guide_roughness = image_or<sg::pixel_format::r16_float>(d.guide_roughness, self->_guide_roughness_stand_in),
            .frame_output = image_or<sg::pixel_format::rgba16_float>(d.frame_output, self->_frame_output_stand_in),
            .guide_motion = image_or<sg::pixel_format::rg32_float>(d.guide_motion, self->_guide_motion_stand_in),
            .frame_diffuse = image_or<sg::pixel_format::rgba16_float>(d.frame_diffuse, self->_frame_diffuse_stand_in),
            .frame_specular = image_or<sg::pixel_format::rgba16_float>(d.frame_specular, self->_frame_specular_stand_in),
            .guide_hit_distance
            = image_or<sg::pixel_format::rg32_float>(d.guide_hit_distance, self->_guide_hit_distance_stand_in),
            .lights = lights.reinterpret_as<tracer::light_record>().as_readonly_buffer(),
            .primary_depth = image_or<sg::pixel_format::r32_float>(d.primary_depth, self->_primary_depth_stand_in),
        });

    cmd.raytracing.bind_pipeline(*variant->pipeline);
    cmd.raytracing.bind_group(0, *group);
    cmd.raytracing.bind_group(1, *d.bindless->group());

    // An array the code indexes and nobody declared is logged and barriered whole, so every table is declared.
    d.bindless->declare_raytracing_access(cmd);

    cmd.raytracing.dispatch_rays(*variant->table, sg::raygen_index(0), d.output.width(), d.output.height());
    return sg::routine_outcome::executed;
}
} // namespace sv
