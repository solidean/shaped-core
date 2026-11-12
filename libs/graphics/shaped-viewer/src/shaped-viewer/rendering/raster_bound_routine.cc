#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sgl_modules/raster_bound.hh>
#include <sgl_modules/raster_bound_indirect.hh>
#include <sgl_modules/raster_bound_reflections.hh>
#include <sgl_modules/raster_bound_shadow_maps.hh>
#include <sgl_modules/raster_bound_shadows.hh>
#include <sgl_modules/slug.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/rendering/raster_bound_routine.hh>
#include <shaped-viewer/resources/material_shader_cache.hh>
#include <shaped-viewer/scene/light.hh>
#include <shaped-viewer/shader_library.hh>
#include <sv_shaders.hh>

namespace sv
{
namespace
{
/// How many frames a textured draw's cached group outlives its last use.
constexpr u64 texture_group_idle_frames = 120;

/// The faces of a quadric's box that are not drawn: the near ones, as the raster path's boxes leave them.
constexpr auto quadric_box_cull = sg::cull_mode::front;

namespace bound_module = sv::shaders::raster_bound;
namespace shadow_module = sv::shaders::raster_bound_shadows;
namespace map_module = sv::shaders::raster_bound_shadow_maps;
namespace reflection_module = sv::shaders::raster_bound_reflections;
namespace indirect_module = sv::shaders::raster_bound_indirect;
namespace tracer = sv::shaders::tracer;

/// Reversed, `near / depth` cleared to 0 at infinity, as the bindless mode's.
constexpr auto depth_format = sg::pixel_format::depth32_float;

/// `raster_bound_shadow_maps`' atlas: a tile per light, `tiles_per_row` to a row, the first `max_mapped_lights` lights.
constexpr auto shadow_tile = 1024;
constexpr auto shadow_tiles_per_row = 4;
constexpr auto max_mapped_lights = isize(16);

/// Whether `r` gets a shadow map, as `raster_bound_shadow_maps.is_mapped` decides.
[[nodiscard]] bool is_mapped(tracer::light_record const& r)
{
    return r.path == u32(light_path::area) && (r.flags & light_flag_casts_no_shadow) == 0;
}

/// The entry every generated bound pixel stage declares, indexed and pulled.
constexpr auto bound_entry = cc::string_view("sv_raster_bound");
constexpr auto bound_pulled_entry = cc::string_view("sv_raster_bound_pulled");

/// `raster_bound.targets` as a pipeline states it.
[[nodiscard]] bound_module::targets::states target_states()
{
    return {.color = {.format = sg::pixel_format::rgba32_float},
            .primary_depth = {.format = sg::pixel_format::r32_float},
            .motion = {.format = sg::pixel_format::rg16_float}};
}

/// `bg`'s lanes, as the group's constant buffer holds them.
[[nodiscard]] cc::fixed_array<tg::vec4f, background_gpu::lane_count> background_lanes_of(background_gpu const& bg)
{
    static_assert(sizeof(background_gpu) == sizeof(tg::vec4f) * background_gpu::lane_count);
    auto lanes = cc::fixed_array<tg::vec4f, background_gpu::lane_count>();
    cc::memcpy(lanes.data(), &bg, sizeof(background_gpu));
    return lanes;
}

/// The context's first shader format the library builds SGL into, as a compile of a package's asset would pick.
[[nodiscard]] cc::optional<sg::shader_format> sgl_format_for(sg::context const& ctx, slib::shader_library const& lib)
{
    for (auto const f : ctx.accepted_shader_formats())
        if (lib.can_compile(slib::shader_language::sgl, f))
            return f;
    return {};
}
} // namespace

cc::shared_async<cc::unit> raster_bound_routine::init_once(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();
    _stand_ins = stand_ins{
        .lights = ctx.persistent.create_buffer_from_data(cc::vector<tracer::light_record>{tracer::light_record{}},
                                                         sg::buffer_usage::readonly_buffer),
        .bytes = ctx.persistent.create_buffer<byte>(16, sg::buffer_usage::readonly_buffer),
        .no_occlusion
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::r8_unorm,
                                            .width = 1,
                                            .height = 1,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst}),
    };
    ctx.upload.bytes_to_texture(_stand_ins.no_occlusion.raw(), cc::make_pinned_data(cc::vector<byte>{byte(255)}));
    _stand_ins.no_reflection
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::rgba16_float,
                                            .width = 1,
                                            .height = 1,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    ctx.upload.bytes_to_texture(_stand_ins.no_reflection.raw(),
                                cc::make_pinned_data(cc::vector<byte>::create_filled(8, byte(0))));
    _stand_ins.no_reflection_depth
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::r32_float,
                                            .width = 1,
                                            .height = 1,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    ctx.upload.bytes_to_texture(_stand_ins.no_reflection_depth.raw(),
                                cc::make_pinned_data(cc::vector<byte>::create_filled(4, byte(0))));
    _stand_ins.no_ids
        = ctx.persistent.create_texture_2d({.format = sg::pixel_format::r32_uint,
                                            .width = 1,
                                            .height = 1,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::copy_dst});
    ctx.upload.bytes_to_texture(_stand_ins.no_ids.raw(),
                                cc::make_pinned_data(cc::vector<byte>::create_filled(4, byte(0))));
    _stand_ins.decal_curves = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba16_float, .width = 1, .height = 1, .usage = sg::texture_usage::texture});
    _stand_ins.decal_bands = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rg16_uint, .width = 1, .height = 1, .usage = sg::texture_usage::texture});
    co_return;
}

cc::shared_async<cc::unit> raster_bound_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // A reload re-generates every material's pixel stage against modules that may have moved.
    _surfaces.clear();
    _texture_groups.clear();
    _environment = nullptr;
    _depth_only = nullptr;
    _quadric_vs = {};
    _quadric_depth_only = nullptr;
    _occlusion = nullptr;
    _occlusion_accumulate = nullptr;
    _occlusion_blur = nullptr;
    _reflection = nullptr;
    _indirect = nullptr;
    _indirect_accumulate = nullptr;
    _indirect_blur = nullptr;
    _reflection_trace = nullptr;
    _shadow = nullptr;
    _caster = nullptr;
    _shadow_map = nullptr;
    _shadow_atlas_key = 0;
    _shadow_tlas = nullptr;
    _shadow_tlas_key = 0;

    // Without `primitive_index` a pixel cannot ask for its triangle, so the draws are pulled and the vertex stage hands it over.
    _pulled = !ctx.supports(sg::feature::primitive_index);
    auto const surface_vs = _pulled ? shaders::raster_bound_pass::bound_pulled_vs->acquire(ctx)
                                    : shaders::raster_bound_pass::bound_vs->acquire(ctx);
    auto const environment_vs = shaders::raster_bound_pass::bound_environment_vs->acquire(ctx);
    auto const environment_ps = shaders::raster_bound_pass::bound_environment_ps->acquire(ctx);
    co_await cc::async_settled(surface_vs);
    co_await cc::async_settled(environment_vs);
    co_await cc::async_settled(environment_ps);

    auto const* const compiled_surface_vs = surface_vs->try_value();
    auto const* const compiled_environment_vs = environment_vs->try_value();
    auto const* const compiled_environment_ps = environment_ps->try_value();
    if (compiled_surface_vs == nullptr || compiled_environment_vs == nullptr || compiled_environment_ps == nullptr)
    {
        // Said once here, since a failed routine declines every frame without a word after it.
        auto const reason = [](auto const& stage) -> cc::string
        {
            auto const* const error = stage->try_error();
            return error != nullptr ? error->underlying().to_string() : cc::string("compiled");
        };
        CC_LOG_ERROR("the bound raster mode's fixed stages did not compile, so no bound frame draws: vertex {}; "
                     "environment vertex {}; environment pixel {}",
                     reason(surface_vs), reason(environment_vs), reason(environment_ps));
        fail_init();
        co_return;
    }
    _surface_vs = *compiled_surface_vs;

    // Behind every surface: drawn at depth 0, tested against the far plane the clear left, and writing no depth of its own.
    auto const environment = ctx.cached.acquire_raster_pipeline({
        .layout = shaders::raster_bound_pass::bound_environment_ps.acquire_layout(ctx),
        .vertex_shader = *compiled_environment_vs,
        .fragment_shader = *compiled_environment_ps,
        .topology = sg::primitive_topology::triangle_list, // no vertex input: the vertex index
        .rasterization = {.cull = sg::cull_mode::none},
        .depth_stencil = {.depth_test = true, .depth_write = false, .depth_compare = sg::compare_op::greater_equal},
        .color_targets = target_states(),
        .depth_stencil_format = depth_format,
    });
    co_await cc::async_settled(environment);
    auto const* const built = environment->try_value();
    if (built == nullptr)
    {
        CC_LOG_ERROR("the bound raster mode's environment pipeline did not build, so no bound frame draws: {}",
                     environment->try_error()->underlying().to_string());
        fail_init();
        co_return;
    }
    _environment = *built;

    // The shading draws' own vertex stage, so the depth it lays down is the very depth their test meets.
    // It writes the instance each pixel shows too, which the traced reflections read the material of.
    auto const prepass_ps = shaders::raster_bound_pass::bound_prepass_ps->acquire(ctx);
    co_await cc::async_settled(prepass_ps);
    if (prepass_ps->try_value() == nullptr)
    {
        CC_LOG_ERROR("the bound raster mode's prepass pixel stage did not compile, so no bound frame draws: {}",
                     prepass_ps->try_error()->underlying().to_string());
        fail_init();
        co_return;
    }
    auto const depth_only = ctx.cached.acquire_raster_pipeline({
        .layout = ctx.cached.acquire_pipeline_layout<bound_module::bound>(),
        .vertex_shader = _surface_vs,
        .fragment_shader = *prepass_ps->try_value(),
        .topology = sg::primitive_topology::triangle_list,
        .rasterization = {.cull = sg::cull_mode::none},
        .depth_stencil = {.depth_test = true, .depth_write = true, .depth_compare = sg::compare_op::greater},
        .color_targets = bound_module::prepass_target::states{.instance = {.format = sg::pixel_format::r32_uint},
                                                              .normal = {.format = sg::pixel_format::rgba16_float}},
        .depth_stencil_format = depth_format,
    });
    co_await cc::async_settled(depth_only);
    if (auto const* const prepass = depth_only->try_value(); prepass != nullptr)
        _depth_only = *prepass;
    else
        CC_LOG_ERROR("the bound raster mode's depth prepass did not build, so a bound frame shades its overdraw: {}",
                     depth_only->try_error()->underlying().to_string());

    // A quadric batch's boxes, and the prepass of the surfaces their pixels solve.
    // Their far faces alone, so a camera inside a box still covers it and each pixel solves the surface once.
    auto const quadric_vs = shaders::raster_bound_pass::bound_quadric_vs->acquire(ctx);
    auto const quadric_prepass_ps = shaders::raster_bound_pass::bound_quadric_prepass_ps->acquire(ctx);
    co_await cc::async_settled(quadric_vs);
    co_await cc::async_settled(quadric_prepass_ps);
    if (quadric_vs->try_value() == nullptr || quadric_prepass_ps->try_value() == nullptr)
        CC_LOG_ERROR(
            "the bound raster mode's quadric stages did not compile, so no quadric batch draws: {}",
            (quadric_vs->try_value() == nullptr ? quadric_vs : quadric_prepass_ps)->try_error()->underlying().to_string());
    else
    {
        _quadric_vs = *quadric_vs->try_value();
        auto const quadric_depth_only = ctx.cached.acquire_raster_pipeline({
            .layout = ctx.cached.acquire_pipeline_layout<bound_module::bound>(),
            .vertex_shader = _quadric_vs.value(),
            .fragment_shader = *quadric_prepass_ps->try_value(),
            .topology = sg::primitive_topology::triangle_list,
            .rasterization = {.cull = quadric_box_cull},
            .depth_stencil = {.depth_test = true, .depth_write = true, .depth_compare = sg::compare_op::greater_equal},
            .color_targets
            = bound_module::quadric_prepass_target::states{.instance = {.format = sg::pixel_format::r32_uint},
                                                           .normal = {.format = sg::pixel_format::rgba16_float}},
            .depth_stencil_format = depth_format,
        });
        co_await cc::async_settled(quadric_depth_only);
        if (auto const* const built_quadric = quadric_depth_only->try_value(); built_quadric != nullptr)
            _quadric_depth_only = *built_quadric;
        else
            CC_LOG_ERROR("the bound raster mode's quadric prepass did not build, so its quadrics cast nothing on "
                         "screen: {}",
                         quadric_depth_only->try_error()->underlying().to_string());
    }

    // The occlusion passes draw the environment's covering triangle into a half-size target, with no depth.
    auto const occlusion_ps = shaders::raster_bound_pass::bound_ao_ps->acquire(ctx);
    auto const accumulate_ps = shaders::raster_bound_pass::bound_ao_accumulate_ps->acquire(ctx);
    auto const blur_ps = shaders::raster_bound_pass::bound_ao_blur_ps->acquire(ctx);
    co_await cc::async_settled(occlusion_ps);
    co_await cc::async_settled(accumulate_ps);
    co_await cc::async_settled(blur_ps);
    auto const occlusion_pipeline
        = [&](sg::pipeline_layout_handle layout, sg::compiled_shader const& ps, auto const& states)
    {
        return ctx.cached.acquire_raster_pipeline({
            .layout = cc::move(layout),
            .vertex_shader = *compiled_environment_vs,
            .fragment_shader = ps,
            .topology = sg::primitive_topology::triangle_list,
            .rasterization = {.cull = sg::cull_mode::none},
            .color_targets = states,
        });
    };
    auto const ao_states = bound_module::ao_target::states{.ao = {.format = sg::pixel_format::r8_unorm}};
    auto const history_states = bound_module::ao_history_target::states{.ao = {.format = sg::pixel_format::rgba32_float}};
    auto const resolved_states = bound_module::ao_resolved_target::states{.ao = {.format = sg::pixel_format::rg32_float}};
    if (occlusion_ps->try_value() != nullptr && accumulate_ps->try_value() != nullptr && blur_ps->try_value() != nullptr)
    {
        auto const occlusion = occlusion_pipeline(shaders::raster_bound_pass::bound_ao_ps.acquire_layout(ctx),
                                                  *occlusion_ps->try_value(), ao_states);
        auto const accumulate = occlusion_pipeline(shaders::raster_bound_pass::bound_ao_accumulate_ps.acquire_layout(ctx),
                                                   *accumulate_ps->try_value(), history_states);
        auto const blur = occlusion_pipeline(shaders::raster_bound_pass::bound_ao_blur_ps.acquire_layout(ctx),
                                             *blur_ps->try_value(), resolved_states);
        co_await cc::async_settled(occlusion);
        co_await cc::async_settled(accumulate);
        co_await cc::async_settled(blur);
        if (occlusion->try_value() != nullptr && accumulate->try_value() != nullptr && blur->try_value() != nullptr)
        {
            _occlusion = *occlusion->try_value();
            _occlusion_accumulate = *accumulate->try_value();
            _occlusion_blur = *blur->try_value();
        }
    }
    if (_occlusion == nullptr)
        CC_LOG_ERROR("the bound raster mode's ambient occlusion did not build, so a bound frame draws without it");

    // The reflections draw the covering triangle too, into two half-size targets.
    auto const reflection_ps = shaders::raster_bound_pass::bound_reflection_ps->acquire(ctx);
    co_await cc::async_settled(reflection_ps);
    if (reflection_ps->try_value() != nullptr)
    {
        auto const reflection = occlusion_pipeline(
            shaders::raster_bound_pass::bound_reflection_ps.acquire_layout(ctx), *reflection_ps->try_value(),
            reflection_module::reflection_target::states{.reflection = {.format = sg::pixel_format::rgba16_float},
                                                         .depth = {.format = sg::pixel_format::r32_float}});
        co_await cc::async_settled(reflection);
        if (reflection->try_value() != nullptr)
            _reflection = *reflection->try_value();
    }
    if (_reflection == nullptr)
        CC_LOG_ERROR("the bound raster mode's reflections did not build, so a bound frame draws without them");

    // The shadow maps: each light's depth through the shading draws' own vertex fetch, then a half-size pass reading them.
    {
        auto const caster_vs = _pulled ? shaders::raster_bound_shadow_map_pass::bound_caster_pulled_vs->acquire(ctx)
                                       : shaders::raster_bound_shadow_map_pass::bound_caster_vs->acquire(ctx);
        auto const map_vs = shaders::raster_bound_shadow_map_pass::bound_shadow_map_vs->acquire(ctx);
        auto const map_ps = shaders::raster_bound_shadow_map_pass::bound_shadow_map_ps->acquire(ctx);
        co_await cc::async_settled(caster_vs);
        co_await cc::async_settled(map_vs);
        co_await cc::async_settled(map_ps);
        if (caster_vs->try_value() != nullptr && map_vs->try_value() != nullptr && map_ps->try_value() != nullptr)
        {
            auto const caster_layout
                = _pulled ? shaders::raster_bound_shadow_map_pass::bound_caster_pulled_vs.acquire_layout(ctx)
                          : shaders::raster_bound_shadow_map_pass::bound_caster_vs.acquire_layout(ctx);
            auto const caster = ctx.cached.acquire_raster_pipeline({
                .layout = caster_layout,
                .vertex_shader = *caster_vs->try_value(),
                .topology = sg::primitive_topology::triangle_list,
                .rasterization = {.cull = sg::cull_mode::none},
                .depth_stencil = {.depth_test = true, .depth_write = true, .depth_compare = sg::compare_op::greater},
                .depth_stencil_format = depth_format,
            });
            auto const map = ctx.cached.acquire_raster_pipeline({
                .layout = shaders::raster_bound_shadow_map_pass::bound_shadow_map_ps.acquire_layout(ctx),
                .vertex_shader = *map_vs->try_value(),
                .fragment_shader = *map_ps->try_value(),
                .topology = sg::primitive_topology::triangle_list,
                .rasterization = {.cull = sg::cull_mode::none},
                .color_targets
                = map_module::shadow_map_target::states{.visibility = {.format = sg::pixel_format::r8_unorm}},
            });
            co_await cc::async_settled(caster);
            co_await cc::async_settled(map);
            if (caster->try_value() != nullptr && map->try_value() != nullptr)
            {
                _caster = *caster->try_value();
                _shadow_map = *map->try_value();
            }
        }
        if (_caster == nullptr)
            CC_LOG_ERROR("the bound raster mode's shadow maps did not build, so a bound frame with maps draws "
                         "unshadowed");
    }

    // The shadow rays need an inline trace, which a device without `ray_query` cannot run; it draws unshadowed.
    if (ctx.supports(sg::feature::ray_query))
    {
        auto const shadow_vs = shaders::raster_bound_shadow_pass::bound_shadow_vs->acquire(ctx);
        auto const shadow_ps = shaders::raster_bound_shadow_pass::bound_shadow_ps->acquire(ctx);
        co_await cc::async_settled(shadow_vs);
        co_await cc::async_settled(shadow_ps);
        if (shadow_vs->try_value() != nullptr && shadow_ps->try_value() != nullptr)
        {
            auto const shadow = ctx.cached.acquire_raster_pipeline({
                .layout = shaders::raster_bound_shadow_pass::bound_shadow_ps.acquire_layout(ctx),
                .vertex_shader = *shadow_vs->try_value(),
                .fragment_shader = *shadow_ps->try_value(),
                .topology = sg::primitive_topology::triangle_list,
                .rasterization = {.cull = sg::cull_mode::none},
                .color_targets
                = shadow_module::shadow_target::states{.visibility = {.format = sg::pixel_format::r8_unorm}},
            });
            co_await cc::async_settled(shadow);
            if (shadow->try_value() != nullptr)
                _shadow = *shadow->try_value();
        }
        if (_shadow == nullptr)
            CC_LOG_ERROR("the bound raster mode's shadow rays did not build, so a bound frame draws unshadowed");

        auto const indirect_vs = shaders::raster_bound_indirect_pass::bound_indirect_vs->acquire(ctx);
        auto const indirect_ps = shaders::raster_bound_indirect_pass::bound_indirect_ps->acquire(ctx);
        auto const indirect_accumulate_ps
            = shaders::raster_bound_indirect_pass::bound_indirect_accumulate_ps->acquire(ctx);
        auto const indirect_blur_ps = shaders::raster_bound_indirect_pass::bound_indirect_blur_ps->acquire(ctx);
        auto const reflection_trace_ps = shaders::raster_bound_indirect_pass::bound_reflection_trace_ps->acquire(ctx);
        co_await cc::async_settled(reflection_trace_ps);
        co_await cc::async_settled(indirect_vs);
        co_await cc::async_settled(indirect_ps);
        co_await cc::async_settled(indirect_accumulate_ps);
        co_await cc::async_settled(indirect_blur_ps);
        if (indirect_vs->try_value() != nullptr && indirect_ps->try_value() != nullptr
            && indirect_accumulate_ps->try_value() != nullptr && indirect_blur_ps->try_value() != nullptr)
        {
            auto const states
                = indirect_module::indirect_target::states{.radiance = {.format = sg::pixel_format::rgba16_float}};
            auto const indirect_pipeline = [&](sg::pipeline_layout_handle layout, sg::compiled_shader const& ps)
            {
                return ctx.cached.acquire_raster_pipeline({
                    .layout = cc::move(layout),
                    .vertex_shader = *indirect_vs->try_value(),
                    .fragment_shader = ps,
                    .topology = sg::primitive_topology::triangle_list,
                    .rasterization = {.cull = sg::cull_mode::none},
                    .color_targets = states,
                });
            };
            auto const traced = indirect_pipeline(
                shaders::raster_bound_indirect_pass::bound_indirect_ps.acquire_layout(ctx), *indirect_ps->try_value());
            auto const blur
                = indirect_pipeline(shaders::raster_bound_indirect_pass::bound_indirect_blur_ps.acquire_layout(ctx),
                                    *indirect_blur_ps->try_value());
            auto const accumulate = ctx.cached.acquire_raster_pipeline({
                .layout = shaders::raster_bound_indirect_pass::bound_indirect_accumulate_ps.acquire_layout(ctx),
                .vertex_shader = *indirect_vs->try_value(),
                .fragment_shader = *indirect_accumulate_ps->try_value(),
                .topology = sg::primitive_topology::triangle_list,
                .rasterization = {.cull = sg::cull_mode::none},
                .color_targets
                = indirect_module::indirect_history_target::states{.radiance = {.format = sg::pixel_format::rgba16_float},
                                                                   .depth = {.format = sg::pixel_format::r32_float}},
            });
            co_await cc::async_settled(traced);
            co_await cc::async_settled(accumulate);
            co_await cc::async_settled(blur);
            if (traced->try_value() != nullptr && accumulate->try_value() != nullptr && blur->try_value() != nullptr)
            {
                _indirect = *traced->try_value();
                _indirect_accumulate = *accumulate->try_value();
                _indirect_blur = *blur->try_value();
            }
            if (reflection_trace_ps->try_value() != nullptr)
            {
                auto const reflection_trace = ctx.cached.acquire_raster_pipeline({
                    .layout = shaders::raster_bound_indirect_pass::bound_reflection_trace_ps.acquire_layout(ctx),
                    .vertex_shader = *indirect_vs->try_value(),
                    .fragment_shader = *reflection_trace_ps->try_value(),
                    .topology = sg::primitive_topology::triangle_list,
                    .rasterization = {.cull = sg::cull_mode::none},
                    .color_targets
                    = indirect_module::traced_reflection_target::states{.reflection
                                                                        = {.format = sg::pixel_format::rgba16_float},
                                                                        .history
                                                                        = {.format = sg::pixel_format::rgba16_float},
                                                                        .depth = {.format = sg::pixel_format::r32_float}},
                });
                co_await cc::async_settled(reflection_trace);
                if (reflection_trace->try_value() != nullptr)
                    _reflection_trace = *reflection_trace->try_value();
            }
        }
        if (_indirect == nullptr)
            CC_LOG_ERROR("the bound raster mode's indirect rays did not build, so a bound frame lights by the "
                         "environment");
    }
    co_return;
}

sg::raster_pipeline_handle raster_bound_routine::_surface_pipeline_of(sg::context& ctx, material_permutation const* p)
{
    auto& s = _surfaces.entry(p->key).get_or_emplace(surface_pipeline{});
    if (s.pipeline != nullptr)
        return s.pipeline;
    if (s.failed)
        return nullptr;

    auto const quadrics = p->kind == geometry_kind::quadrics;
    if (quadrics && !_quadric_vs.has_value())
    {
        s.failed = true;
        return nullptr;
    }

    if (!s.shader.is_valid())
    {
        auto const& source = _pulled ? p->bound_pulled_source : p->bound_source;
        CC_ASSERT(!source.empty(), "every permutation a bound frame draws has a bound pixel stage");
        auto lib = acquire_shader_library();
        auto const format = lib.has_value() ? sgl_format_for(ctx, *lib.value()) : cc::optional<sg::shader_format>();
        if (!format.has_value())
        {
            CC_LOG_ERROR("no shader library builds {}'s bound raster stage into a format this context accepts", p->label);
            s.failed = true;
            return nullptr;
        }
        s.shader = lib.value()->compile_source(source, sg::shader_stage::fragment,
                                               _pulled ? bound_pulled_entry : bound_entry, format.value(),
                                               {.language = slib::shader_language::sgl, .label = p->label});
        (void)ctx.backlog.start(s.shader);
        return nullptr;
    }

    if (!s.pending.is_valid())
    {
        if (!s.shader->is_ready())
            return nullptr;
        auto const* const compiled = s.shader->try_value();
        if (compiled == nullptr)
        {
            CC_LOG_ERROR("the bound stage of {} did not compile, so its geometry draws as the stand-in: {}", p->label,
                         s.shader->try_error()->underlying().to_string());
            s.failed = true;
            return nullptr;
        }

        // A textured material's group 1 is generated per permutation, so its layout is what the compiled stage declares.
        auto texture_bindings = cc::vector<sg::binding>();
        s.slot_bound = cc::vector<bool>::create_filled(p->bound_samplers.size(), false);
        for (auto const& b : compiled->bindings)
            if (cc::string_view(b.name).starts_with("sv_textures."))
            {
                texture_bindings.push_back(b);
                // `sv_textures.t<k>` or `sv_textures.s<k>`: slot k is read.
                auto k = isize(0);
                for (auto const c : cc::string_view(b.name).subview(cc::string_view("sv_textures.t").size()))
                    k = k * 10 + isize(c - '0');
                if (k < s.slot_bound.size())
                    s.slot_bound[k] = true;
            }
        // The decal atlas is group 1 of every material's stage, and its textures group 2 where it has any.
        auto groups = cc::small_vector<sg::binding_group_layout_handle, sg::max_binding_groups>();
        groups.push_back(ctx.cached.acquire_binding_group_layout<bound_module::bound>());
        groups.push_back(ctx.cached.acquire_binding_group_layout<sgl_modules::slug::tables>());
        if (!texture_bindings.empty())
        {
            s.textures_layout = ctx.cached.acquire_binding_group_layout(texture_bindings, {});
            groups.push_back(s.textures_layout);
        }
        auto const layout = ctx.cached.acquire_pipeline_layout({.groups = cc::move(groups)});

        // Two-sided like the tracer, which shades a triangle seen from behind through its turned normal.
        // A quadric's box draws its far faces alone, as its prepass does.
        auto const description = sg::raster_pipeline_description{
            .layout = layout,
            .vertex_shader = quadrics ? _quadric_vs.value() : _surface_vs,
            .fragment_shader = *compiled,
            .topology = sg::primitive_topology::triangle_list,
            .rasterization = {.cull = quadrics ? quadric_box_cull : sg::cull_mode::none},
            .depth_stencil = {.depth_test = true, .depth_write = true, .depth_compare = sg::compare_op::greater_equal},
            .color_targets = target_states(),
            .depth_stencil_format = depth_format,
        };

        s.pending = ctx.cached.acquire_raster_pipeline(description);
        (void)ctx.backlog.start(s.pending);
        return nullptr;
    }

    if (!s.pending->is_ready())
        return nullptr;

    auto const* const built = s.pending->try_value();
    if (built == nullptr)
    {
        CC_LOG_ERROR("the bound stage of {} did not build into a pipeline: {}", p->label,
                     s.pending->try_error()->underlying().to_string());
        s.failed = true;
        return nullptr;
    }
    s.pipeline = *built;
    return s.pipeline;
}

sg::routine_outcome raster_bound_routine::execute(sg::command_list& cmd, raster_bound_frame_desc const& d)
{
    CC_RECORD_SCOPE("sv.raster_bound");

    // Exclusive: _surface_pipeline_of writes the pipeline map.
    auto self = try_acquire_exclusive(cmd);
    if (!self.is_ready() || self->_environment == nullptr)
        return sg::routine_outcome::declined;
    auto& ctx = cmd.context();

    CC_ASSERT(d.color.raw() != nullptr, "raster_bound_routine: no color target bound");
    CC_ASSERT(d.color.raw()->description().format == sg::pixel_format::rgba32_float,
              "raster_bound_routine: the color target is rgba32_float, the layer's accumulation slot");
    CC_ASSERT(d.vertex_pool.raw() != nullptr && d.index_pool.raw() != nullptr, "raster_bound_routine: a frame binds "
                                                                               "the mesh manager's pools");
    auto const extent = tg::vec2i(d.color.width(), d.color.height());

    // Started whether or not a substitution ends up needing it.
    auto const fallback = d.fallback != nullptr ? self->_surface_pipeline_of(ctx, d.fallback) : nullptr;

    auto pipelines = cc::vector<sg::raster_pipeline_handle>();
    pipelines.reserve(d.permutations.size());
    auto bound = cc::vector<material_permutation const*>();
    bound.reserve(d.permutations.size());
    auto substituted = false;
    auto pending = false;
    for (auto const* p : d.permutations)
    {
        CC_ASSERT(p != nullptr, "a bound frame names a permutation the shader cache does not hold");
        // A quadric batch has no stand-in to draw its boxes with, since the fallback is a triangle material: it waits,
        // which the report counts as a substitution, so a frame missing it does not read as converged.
        if (p->kind == geometry_kind::quadrics)
        {
            auto pipeline = self->_surface_pipeline_of(ctx, p);
            if (pipeline == nullptr && !self->_surfaces.get_ptr(p->key)->failed)
            {
                pending = true;
                substituted = true;
            }
            pipelines.push_back(cc::move(pipeline));
            bound.push_back(p);
            continue;
        }
        auto pipeline = self->_surface_pipeline_of(ctx, p);
        if (pipeline == nullptr)
        {
            // _surface_pipeline_of made the entry, so it is there to ask whether the compile settled.
            pending = pending || !self->_surfaces.get_ptr(p->key)->failed;
            pipeline = fallback;
            p = d.fallback;
            substituted = true;
        }
        if (pipeline == nullptr)
            return sg::routine_outcome::declined;
        pipelines.push_back(cc::move(pipeline));
        bound.push_back(p);
    }

    auto const bytes_or_stand_in = [&](sg::buffer<byte> const& b)
    { return (b.raw() != nullptr ? b : self->_stand_ins.bytes).as_readonly_buffer(); };
    CC_ASSERT(d.decal_lanes.size() <= raster_bound_frame_desc::decal_lane_count, "raster_bound_routine: the decals "
                                                                                 "fit the lanes group 0 holds");
    auto decal_lanes = cc::fixed_array<tg::vec4f, raster_bound_frame_desc::decal_lane_count>();
    for (auto i = isize(0); i < decal_lanes.size(); ++i)
        decal_lanes[i] = i < d.decal_lanes.size() ? d.decal_lanes[i] : tg::vec4f(0.0f);
    auto const frame = ctx.transient.create_buffer_from_pod(cmd, d.frame, sg::buffer_usage::readonly_buffer);
    // Group 0 over a given occlusion and shadow share: the passes computing them bind the white stand-in, since they
    // write the real ones; `history_valid` is what an accumulation pass reads, whose signal's history it is.
    auto const white = self->_stand_ins.no_occlusion;
    auto const no_reflection = self->_stand_ins.no_reflection;
    auto const no_reflection_depth = self->_stand_ins.no_reflection_depth;
    // What group 0 binds of the screen-space signals the frame computes; each pass computing one binds the stand-ins.
    struct signals
    {
        sg::texture_2d occlusion;
        sg::texture_2d shadow;
        sg::texture_2d reflection;
        sg::texture_2d reflection_depth;
        sg::texture_2d indirect;
        f32 indirect_scale = 1.0f;
        sg::texture_2d ids;
    };
    auto const group_over = [&](signals const& s, bool history_valid)
    {
        return ctx.transient.create_binding_group(
            cmd, ctx.cached.acquire_binding_group_layout<bound_module::bound>(),
            bound_module::bound{
                .frame = frame.as_readonly_buffer(),
                .instances = d.instance_table.as_readonly_buffer(),
                .lights = (d.lights.raw() != nullptr ? d.lights : self->_stand_ins.lights).as_readonly_buffer(),
                .background = background_lanes_of(d.background),
                .vertices = d.vertex_pool.reinterpret_as<byte>().as_readonly_buffer(),
                .indices = d.index_pool.reinterpret_as<byte>().as_readonly_buffer(),
                .params = bytes_or_stand_in(d.parameter_pool),
                .attributes = bytes_or_stand_in(d.attribute_pool),
                .ao = s.occlusion.as_texture_view(),
                .shadow = s.shadow.as_texture_view(),
                .reflection = s.reflection.as_texture_view(),
                .reflection_depth = s.reflection_depth.as_texture_view(),
                .indirect = s.indirect.as_texture_view(),
                .indirect_settings = tg::vec4f(s.indirect_scale, 0.0f, 0.0f, 0.0f),
                .ids = s.ids.as_texture_view(),
                .ao_settings = tg::vec4f(d.ambient_occlusion.radius, d.ambient_occlusion.thickness,
                                         d.ambient_occlusion.intensity, d.ambient_occlusion.direct_share),
                // Up to 32 frames averaged while the view holds still; motion restarts what it uncovers.
                .ao_temporal = tg::vec4f(f32(d.frame_index % 4096u), history_valid ? 1.0f : 0.0f, 32.0f, 0.0f),
                .decals = decal_lanes,
            });
    };
    auto const no_signals = signals{.occlusion = white,
                                    .shadow = white,
                                    .reflection = no_reflection,
                                    .reflection_depth = no_reflection_depth,
                                    .indirect = no_reflection,
                                    .ids = self->_stand_ins.no_ids};
    auto const unoccluded_group = group_over(no_signals, d.ao_history_valid);

    // A layer with no drawings still has a primary depth written; this frame alone reads it, which is nobody.
    auto const primary_depth = d.primary_depth.raw() != nullptr
                                 ? d.primary_depth
                                 : ctx.transient.create_texture_2d({.format = sg::pixel_format::r32_float,
                                                                    .width = extent[0],
                                                                    .height = extent[1],
                                                                    .usage = sg::texture_usage::render_target});
    auto const motion = d.motion.raw() != nullptr
                          ? d.motion
                          : ctx.transient.create_texture_2d({.format = sg::pixel_format::rg16_float,
                                                             .width = extent[0],
                                                             .height = extent[1],
                                                             .usage = sg::texture_usage::render_target});
    auto const depth
        = ctx.transient.create_texture_2d({.format = depth_format,
                                           .width = extent[0],
                                           .height = extent[1],
                                           .usage = sg::texture_usage::depth_stencil | sg::texture_usage::texture});

    CC_ASSERT(d.draw_textures.empty() || d.draw_textures.size() == d.draws.size(), "raster_bound_routine: one texture "
                                                                                   "list per draw, or none at all");
    auto const pulled = self->_pulled;

    // A frame mesh's own indices are not in the pool its pixel stage reads its corners from.
    // A quadric batch's row draws one box a primitive.
    auto const drawn = [&](raster_draw const& draw)
    {
        if (pipelines[draw.permutation] == nullptr)
            return false;
        if (draw.primitive_count != 0)
            return true;
        return draw.triangle_count != 0 && draw.indices.raw() == nullptr;
    };
    auto const is_quadric = [](raster_draw const& draw) { return draw.primitive_count != 0; };

    auto const issue = [&](auto& into, raster_draw const& draw)
    {
        // 36 corners a box and one instance a primitive; the vertex range starts at 36 times the row, which is how the
        // vertex stage learns it.
        if (is_quadric(draw))
        {
            into.draw({.vertex_range = {.offset = isize(draw.instance) * 36, .size = 36},
                       .instance_range = {.offset = 0, .size = isize(draw.primitive_count)}});
            return;
        }
        // A pulled draw is three corners a triangle, each reading its index out of the pool through the row's record.
        if (pulled)
            into.draw({.vertex_range = {.offset = 0, .size = isize(draw.triangle_count) * 3},
                       .instance_range = {.offset = isize(draw.instance), .size = 1}});
        // The base vertex reaches the vertex stage in `@vertex_index`, so it reads the vertex pool at the index as it stands.
        else
            into.draw_indexed({.index_range = {.offset = isize(draw.first_index), .size = isize(draw.triangle_count) * 3},
                               .instance_range = {.offset = isize(draw.instance), .size = 1},
                               .vertex_offset = i32(draw.base_vertex)});
    };

    // A pass of its own, since a pipeline with no pixel stage cannot draw where color targets are bound.
    // A cutout's depth depends on its material, so it skips the prepass and writes its depth as it shades.
    auto const prepass = d.depth_prepass && self->_depth_only != nullptr;
    auto const ids
        = ctx.transient.create_texture_2d({.format = sg::pixel_format::r32_uint,
                                           .width = extent[0],
                                           .height = extent[1],
                                           .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
    auto const normals
        = ctx.transient.create_texture_2d({.format = sg::pixel_format::rgba16_float,
                                           .width = extent[0],
                                           .height = extent[1],
                                           .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
    auto any_quadric = false;
    for (auto const& draw : d.draws)
        any_quadric = any_quadric || (drawn(draw) && is_quadric(draw));
    if (prepass)
    {
        {
            auto depth_pass = cmd.raster.render_to(bound_module::prepass_target{
                .instance = ids.as_render_target_view().cleared(tg::vec4f(0.0f)),
                .normal = normals.as_render_target_view().cleared(tg::vec4f(0.0f)),
                .depth_stencil = depth.as_depth_stencil_view().cleared(0.0f),
            });
            if (!pulled)
                depth_pass.bind_index_buffer(d.index_pool.as_index_buffer());
            depth_pass.bind_pipeline(*self->_depth_only);
            depth_pass.bind_group(0, *unoccluded_group);
            for (auto const& draw : d.draws)
                if (drawn(draw) && !is_quadric(draw) && bound[draw.permutation]->opacity.kind == material_value_kind::none)
                    issue(depth_pass, draw);
        }

        // A quadric cuts nothing out, so every batch lays its surfaces' depth down too, in a scope of its own: a pixel
        // stage writing depth is a target set of its own.
        if (any_quadric && self->_quadric_depth_only != nullptr)
        {
            auto quadric_pass = cmd.raster.render_to(bound_module::quadric_prepass_target{
                .instance = ids.as_render_target_view().preserved(),
                .normal = normals.as_render_target_view().preserved(),
                .depth_stencil = depth.as_depth_stencil_view().preserved(),
            });
            quadric_pass.bind_pipeline(*self->_quadric_depth_only);
            quadric_pass.bind_group(0, *unoccluded_group);
            for (auto const& draw : d.draws)
                if (drawn(draw) && is_quadric(draw))
                    issue(quadric_pass, draw);
        }
    }

    // A half-size signal over the prepass's depth -- the occlusion, the shadow share -- accumulated over frames where it
    // has a history, then blurred; what the shading reads.
    auto const half = tg::vec2i((extent[0] + 1) / 2, (extent[1] + 1) / 2);
    auto const half_texture = [&]
    {
        return ctx.transient.create_texture_2d({.format = sg::pixel_format::r8_unorm,
                                                .width = half[0],
                                                .height = half[1],
                                                .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
    };
    auto const resolve = [&](sg::texture_2d const& raw, sg::texture_2d const& history_read,
                             sg::texture_2d const& history_write, bool history_valid)
    {
        auto const accumulates = history_write.raw() != nullptr && history_read.raw() != nullptr;
        auto const signal_group = group_over(no_signals, history_valid);
        if (accumulates)
        {
            auto accumulate_pass = cmd.raster.render_to(
                bound_module::ao_history_target{.ao = history_write.as_render_target_view().discarded()});
            accumulate_pass.bind_pipeline(*self->_occlusion_accumulate);
            accumulate_pass.bind_group(0, *signal_group);
            accumulate_pass.bind_group(
                1, *ctx.transient.create_binding_group(
                       cmd, ctx.cached.acquire_binding_group_layout<bound_module::ao_accumulate_source>(),
                       bound_module::ao_accumulate_source{.depth = depth.as_texture_view(),
                                                          .raw = raw.as_texture_view(),
                                                          .history = history_read.as_texture_view(),
                                                          .normals = normals.as_texture_view(),
                                                          .ids = ids.as_texture_view()}));
            accumulate_pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        }
        auto const blurred
            = ctx.transient.create_texture_2d({.format = sg::pixel_format::rg32_float,
                                               .width = half[0],
                                               .height = half[1],
                                               .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
        auto blur_pass
            = cmd.raster.render_to(bound_module::ao_resolved_target{.ao = blurred.as_render_target_view().discarded()});
        blur_pass.bind_pipeline(*self->_occlusion_blur);
        blur_pass.bind_group(0, *signal_group);
        blur_pass.bind_group(
            1, *ctx.transient.create_binding_group(
                   cmd, ctx.cached.acquire_binding_group_layout<bound_module::ao_blur_source>(),
                   bound_module::ao_blur_source{.depth = depth.as_texture_view(),
                                                .raw = (accumulates ? history_write : raw).as_texture_view(),
                                                .normals = normals.as_texture_view()}));
        blur_pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        return blurred;
    };
    auto const resolves = prepass && self->_occlusion != nullptr;

    // Traced indirect light meets what occludes it, so the screen's occlusion would darken it twice; it stands in only
    // where nothing is traced.
    auto const traces_indirect = prepass && d.indirect.enabled && self->_indirect != nullptr && !d.instances.empty();

    // Traced shadows where the device can trace them, the maps where it cannot.
    auto const shadow_technique_used
        = d.shadows.technique == shadow_technique::traced && (self->_shadow == nullptr || d.instances.empty())
            ? shadow_technique::maps
            : d.shadows.technique;

    auto occlusion = white;
    if (resolves && d.ambient_occlusion.enabled && !traces_indirect)
    {
        auto const raw = half_texture();
        {
            auto ao_pass = cmd.raster.render_to(bound_module::ao_target{.ao = raw.as_render_target_view().discarded()});
            ao_pass.bind_pipeline(*self->_occlusion);
            ao_pass.bind_group(0, *unoccluded_group);
            ao_pass.bind_group(1, *ctx.transient.create_binding_group(
                                      cmd, ctx.cached.acquire_binding_group_layout<bound_module::ao_source>(),
                                      bound_module::ao_source{.depth = depth.as_texture_view()}));
            ao_pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        }
        occlusion = resolve(raw, d.ao_history_read, d.ao_history_write, d.ao_history_valid);
    }

    auto shadow = white;

    // A depth map per rect light, redrawn only when what they show changed, then read into the half-size share.
    auto const mapped = cc::min(isize(d.light_records.size()), max_mapped_lights);
    if (resolves && shadow_technique_used == shadow_technique::maps && self->_caster != nullptr && mapped > 0)
    {
        auto const rows = (mapped + shadow_tiles_per_row - 1) / shadow_tiles_per_row;
        auto const atlas_extent = tg::vec2i(shadow_tiles_per_row * shadow_tile, i32(rows) * shadow_tile);
        auto& atlas = self->_shadow_atlas;
        if (atlas.raw() == nullptr || atlas.width() != atlas_extent[0] || atlas.height() != atlas_extent[1])
        {
            atlas = ctx.persistent.create_texture_2d(
                {.format = depth_format,
                 .width = atlas_extent[0],
                 .height = atlas_extent[1],
                 .usage = sg::texture_usage::depth_stencil | sg::texture_usage::texture});
            self->_shadow_atlas_key = 0;
        }
        auto const key = cc::combine_hash(d.caster_key, cc::make_hash(i64(mapped)));
        if (key != self->_shadow_atlas_key)
        {
            // Every opaque draw casts; a cutout or a transmissive surface casts nothing for now.
            auto caster_pass
                = cmd.raster.render_to({.depth_stencil_target = atlas.as_depth_stencil_view().cleared(0.0f)});
            if (!pulled)
                caster_pass.bind_index_buffer(d.index_pool.as_index_buffer());
            caster_pass.bind_pipeline(*self->_caster);
            caster_pass.bind_group(0, *unoccluded_group);
            for (auto i = isize(0); i < mapped; ++i)
            {
                if (!is_mapped(d.light_records[i]))
                    continue;
                auto const corner = tg::pos2i(i32(i % shadow_tiles_per_row) * shadow_tile,
                                              i32(i / shadow_tiles_per_row) * shadow_tile);
                caster_pass.set_viewport(
                    {.offset = tg::pos2f(f32(corner[0]), f32(corner[1])), .size = tg::vec2f(f32(shadow_tile))});
                caster_pass.set_scissor(tg::aabb2i(corner, corner + tg::vec2i(shadow_tile)));
                caster_pass.set_inline_constants(map_module::shadow_caster{.light = u32(i)}.to_block());
                for (auto const& draw : d.draws)
                    if (drawn(draw) && !is_quadric(draw)
                        && bound[draw.permutation]->opacity.kind == material_value_kind::none)
                        issue(caster_pass, draw);
            }
            self->_shadow_atlas_key = key;
        }

        auto const raw = half_texture();
        // Made before the scope opens: its constants upload into the list, which inside a scope would split it.
        auto const map_group = ctx.transient.create_binding_group(
            cmd, ctx.cached.acquire_binding_group_layout<map_module::shadow_map_scene>(),
            map_module::shadow_map_scene{
                .frame = frame.as_readonly_buffer(),
                .lights = (d.lights.raw() != nullptr ? d.lights : self->_stand_ins.lights).as_readonly_buffer(),
                .depth = depth.as_texture_view(),
                .normals = normals.as_texture_view(),
                .atlas = atlas.as_texture_view(),
                .settings = tg::vec4f(d.shadows.softness, 0.0f, 0.0f, 0.0f),
            });
        {
            auto map_pass = cmd.raster.render_to(
                map_module::shadow_map_target{.visibility = raw.as_render_target_view().discarded()});
            map_pass.bind_pipeline(*self->_shadow_map);
            map_pass.bind_group(0, *map_group);
            map_pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        }
        // No history: the maps are the same every frame, so the blur alone resolves the turned taps.
        shadow = resolve(raw, sg::texture_2d(), sg::texture_2d(), false);
    }

    // The scene's TLAS, built from the frame's instances where a pass traces; an unchanged scene keeps last frame's.
    auto const scene_tlas = [&]() -> sg::tlas_handle const&
    {
        // What the build reads of each instance, hashed.
        auto key = u64(0x9e3779b97f4a7c15ull);
        for (auto const& inst : d.instances)
        {
            key = cc::combine_hash(key, cc::make_hash_of_bytes(cc::span<float const>(inst.transform, 12).as_bytes()));
            key = cc::combine_hash(key, cc::make_hash(inst.blas.get(), inst.instance_id, inst.hit_group_offset,
                                                      u32(inst.mask), u32(inst.cull_mode)));
        }
        if (self->_shadow_tlas == nullptr || key != self->_shadow_tlas_key)
        {
            self->_shadow_tlas = cmd.raytracing.build_tlas(d.instances);
            self->_shadow_tlas_key = key;
        }
        return self->_shadow_tlas;
    };

    // One ray a half-size pixel through the scene's TLAS.
    if (resolves && shadow_technique_used == shadow_technique::traced && self->_shadow != nullptr
        && !d.instances.empty() && d.frame.light_count > 0)
    {
        auto const& tlas = scene_tlas();
        auto const raw = half_texture();
        // Made before the scope opens: its constants upload into the list, which inside a scope would split it.
        auto const shadow_group = ctx.transient.create_binding_group(
            cmd, ctx.cached.acquire_binding_group_layout<shadow_module::shadow_scene>(),
            shadow_module::shadow_scene{
                .frame = frame.as_readonly_buffer(),
                .lights = (d.lights.raw() != nullptr ? d.lights : self->_stand_ins.lights).as_readonly_buffer(),
                .depth = depth.as_texture_view(),
                .normals = normals.as_texture_view(),
                .world = tlas->as_view(),
                .instances = d.instance_table.as_readonly_buffer(),
                .vertices = d.vertex_pool.reinterpret_as<byte>().as_readonly_buffer(),
                .settings = tg::vec4f(f32(d.frame_index % 65536u), 0.0f, 0.0f, 0.0f),
            });
        {
            auto shadow_pass = cmd.raster.render_to(
                shadow_module::shadow_target{.visibility = raw.as_render_target_view().discarded()});
            shadow_pass.bind_pipeline(*self->_shadow);
            shadow_pass.bind_group(0, *shadow_group);
            shadow_pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        }
        shadow = resolve(raw, d.shadow_history_read, d.shadow_history_write, d.shadow_history_valid);
    }
    // Mirror rays over the prepass's depth, reading `color` before the shading below overwrites it with this frame.
    auto reflection = no_reflection;
    auto reflection_depth = no_reflection_depth;
    if (prepass && d.reflections.enabled && d.previous_image_valid && d.previous_image.raw() != nullptr
        && self->_reflection != nullptr)
    {
        auto const half_target = [&](sg::pixel_format format)
        {
            return ctx.transient.create_texture_2d(
                {.format = format,
                 .width = half[0],
                 .height = half[1],
                 .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
        };
        reflection = half_target(sg::pixel_format::rgba16_float);
        reflection_depth = half_target(sg::pixel_format::r32_float);
        // Made before the scope opens: its constants upload into the list, which inside a scope would split it.
        auto const source_group = ctx.transient.create_binding_group(
            cmd, ctx.cached.acquire_binding_group_layout<reflection_module::reflection_source>(),
            reflection_module::reflection_source{
                .depth = depth.as_texture_view(),
                .normals = normals.as_texture_view(),
                .previous = d.previous_image.as_texture_view(),
                .settings = tg::vec4f(d.reflections.max_distance, d.reflections.thickness, d.reflections.intensity, 0.0f),
            });
        auto reflection_pass = cmd.raster.render_to(reflection_module::reflection_target{
            .reflection = reflection.as_render_target_view().discarded(),
            .depth = reflection_depth.as_render_target_view().discarded(),
        });
        reflection_pass.bind_pipeline(*self->_reflection);
        reflection_pass.bind_group(0, *unoccluded_group);
        reflection_pass.bind_group(1, *source_group);
        reflection_pass.draw({.vertex_range = {.offset = 0, .size = 3}});
    }

    // One ray a pixel at a reduced size through the scene's TLAS, then blurred within its surfaces.
    auto indirect = no_reflection;
    auto const indirect_scale = cc::max(d.indirect.scale, 1);
    auto const indirect_group_over = [&](sg::tlas_handle const& tlas)
    {
        return ctx.transient.create_binding_group(
            cmd, ctx.cached.acquire_binding_group_layout<indirect_module::indirect_scene>(),
            indirect_module::indirect_scene{
                .frame = frame.as_readonly_buffer(),
                .instances = d.instance_table.as_readonly_buffer(),
                .lights = (d.lights.raw() != nullptr ? d.lights : self->_stand_ins.lights).as_readonly_buffer(),
                .vertices = d.vertex_pool.reinterpret_as<byte>().as_readonly_buffer(),
                .indices = d.index_pool.reinterpret_as<byte>().as_readonly_buffer(),
                .params = bytes_or_stand_in(d.parameter_pool),
                .background = background_lanes_of(d.background),
                .depth = depth.as_texture_view(),
                .normals = normals.as_texture_view(),
                .world = tlas->as_view(),
                .settings = tg::vec4f(f32(d.frame_index % 65536u), f32(indirect_scale),
                                      d.debug_reflection_history ? 1.0f : 0.0f, 0.0f),
            });
    };
    if (traces_indirect)
    {
        auto const& tlas = scene_tlas();
        auto const traced_extent = tg::vec2i((extent[0] + indirect_scale - 1) / indirect_scale,
                                             (extent[1] + indirect_scale - 1) / indirect_scale);
        auto const traced_target = [&]
        {
            return ctx.transient.create_texture_2d(
                {.format = sg::pixel_format::rgba16_float,
                 .width = traced_extent[0],
                 .height = traced_extent[1],
                 .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
        };
        auto const raw = traced_target();
        // Made before the scope opens: its constants upload into the list, which inside a scope would split it.
        auto const indirect_group = indirect_group_over(tlas);
        {
            auto indirect_pass = cmd.raster.render_to(
                indirect_module::indirect_target{.radiance = raw.as_render_target_view().discarded()});
            indirect_pass.bind_pipeline(*self->_indirect);
            indirect_pass.bind_group(0, *indirect_group);
            indirect_pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        }
        // Into the history where the caller keeps one, else into a frame's own, which starts over every frame.
        auto const keeps = d.indirect_history_write.raw() != nullptr && d.indirect_history_read.raw() != nullptr;
        auto const accumulated = keeps ? d.indirect_history_write : traced_target();
        auto const depth_target = [&]
        {
            return ctx.transient.create_texture_2d(
                {.format = sg::pixel_format::r32_float,
                 .width = traced_extent[0],
                 .height = traced_extent[1],
                 .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
        };
        auto const accumulated_depth = keeps ? d.indirect_history_depth_write : depth_target();
        auto const history_valid = keeps && d.indirect_history_valid;
        auto const accumulate_group = ctx.transient.create_binding_group(
            cmd, ctx.cached.acquire_binding_group_layout<indirect_module::indirect_accumulate_source>(),
            indirect_module::indirect_accumulate_source{
                .frame = frame.as_readonly_buffer(),
                .depth = depth.as_texture_view(),
                .raw = raw.as_texture_view(),
                .history = (keeps ? d.indirect_history_read : raw).as_texture_view(),
                .history_depth = (keeps ? d.indirect_history_depth_read : depth_target()).as_texture_view(),
                .normals = normals.as_texture_view(),
                // A still pixel keeps 128 frames, one the camera moves 32: the light changes slowly across a surface.
                .settings = tg::vec4f(f32(indirect_scale), history_valid ? 1.0f : 0.0f, 128.0f, 32.0f),
            });
        {
            auto accumulate_pass = cmd.raster.render_to(indirect_module::indirect_history_target{
                .radiance = accumulated.as_render_target_view().discarded(),
                .depth = accumulated_depth.as_render_target_view().discarded(),
            });
            accumulate_pass.bind_pipeline(*self->_indirect_accumulate);
            accumulate_pass.bind_group(0, *accumulate_group);
            accumulate_pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        }

        indirect = traced_target();
        auto const blur_group = ctx.transient.create_binding_group(
            cmd, ctx.cached.acquire_binding_group_layout<indirect_module::indirect_blur_source>(),
            indirect_module::indirect_blur_source{.accumulated = accumulated.as_texture_view(),
                                                  .depth = accumulated_depth.as_texture_view(),
                                                  .frame = frame.as_readonly_buffer(),
                                                  .normals = normals.as_texture_view(),
                                                  .settings = tg::vec4f(f32(indirect_scale), 0.0f, 0.0f, 0.0f)});
        auto blur_pass = cmd.raster.render_to(
            indirect_module::indirect_target{.radiance = indirect.as_render_target_view().discarded()});
        blur_pass.bind_pipeline(*self->_indirect_blur);
        blur_pass.bind_group(0, *blur_group);
        blur_pass.draw({.vertex_range = {.offset = 0, .size = 3}});
    }

    // A mirror ray where the screen's reflection lost or missed its own, on a surface smooth enough to show one.
    if (reflection.raw() != no_reflection.raw() && d.reflections.traced && self->_reflection_trace != nullptr
        && !d.instances.empty())
    {
        auto const& tlas = scene_tlas();
        auto const scene_group = indirect_group_over(tlas);
        auto const half_target = [&](sg::pixel_format format)
        {
            return ctx.transient.create_texture_2d(
                {.format = format,
                 .width = half[0],
                 .height = half[1],
                 .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
        };
        // Into the history where the caller keeps one, else into a frame's own, which starts over every frame.
        auto const keeps = d.reflection_history_read.raw() != nullptr && d.reflection_history_write.raw() != nullptr;
        auto const history = keeps ? d.reflection_history_write : half_target(sg::pixel_format::rgba16_float);
        auto const history_depth = keeps ? d.reflection_history_depth_write : half_target(sg::pixel_format::r32_float);
        auto const source_group = ctx.transient.create_binding_group(
            cmd, ctx.cached.acquire_binding_group_layout<indirect_module::reflection_trace_source>(),
            indirect_module::reflection_trace_source{
                .ids = ids.as_texture_view(),
                .screen = reflection.as_texture_view(),
                .history
                = (keeps ? d.reflection_history_read : half_target(sg::pixel_format::rgba16_float)).as_texture_view(),
                .history_depth
                = (keeps ? d.reflection_history_depth_read : half_target(sg::pixel_format::r32_float)).as_texture_view(),
                // The shading's trust in a mirror's reflection is gone by roughness 0.45; a still pixel keeps 64
                // frames, a moving one 4, since a reflection moves unlike the surface it is reprojected with.
                .settings = tg::vec4f(0.45f, keeps && d.reflection_history_valid ? 1.0f : 0.0f, 64.0f, 4.0f),
            });
        auto const traced_reflection = half_target(sg::pixel_format::rgba16_float);
        {
            auto trace_pass = cmd.raster.render_to(indirect_module::traced_reflection_target{
                .reflection = traced_reflection.as_render_target_view().discarded(),
                .history = history.as_render_target_view().discarded(),
                .depth = history_depth.as_render_target_view().discarded()});
            trace_pass.bind_pipeline(*self->_reflection_trace);
            trace_pass.bind_group(0, *scene_group);
            trace_pass.bind_group(1, *source_group);
            trace_pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        }
        reflection = traced_reflection;
    }

    auto const computed = signals{.occlusion = occlusion,
                                  .shadow = shadow,
                                  .reflection = reflection,
                                  .reflection_depth = reflection_depth,
                                  .indirect = indirect,
                                  .indirect_scale = f32(indirect_scale),
                                  .ids = prepass ? ids : self->_stand_ins.no_ids};
    auto const group = occlusion.raw() == white.raw() && shadow.raw() == white.raw()
                            && reflection.raw() == no_reflection.raw() && indirect.raw() == no_reflection.raw()
                            && !prepass
                         ? unoccluded_group
                         : group_over(computed, d.ao_history_valid);

    // The decal atlas, group 1 of every material; an atlas nothing was placed in has no textures, so the stand-ins serve.
    auto const has_atlas = d.decal_atlas != nullptr && d.decal_atlas->curve_texture().raw() != nullptr
                        && d.decal_atlas->band_texture().raw() != nullptr;
    auto const tables = ctx.transient.create_binding_group(
        cmd, ctx.cached.acquire_binding_group_layout<sgl_modules::slug::tables>(),
        sgl_modules::slug::tables{
            .curves = (has_atlas ? d.decal_atlas->curve_texture() : self->_stand_ins.decal_curves).as_texture_view(),
            .bands = (has_atlas ? d.decal_atlas->band_texture() : self->_stand_ins.decal_bands).as_texture_view()});

    // A textured permutation's group 2, its textures in the order of its slots; none for an untextured one.
    // Kept across frames, keyed on what it binds, so a texture that lands makes a new one and the old one idles out.
    auto idle = cc::vector<u64>();
    for (auto&& [key, cached] : self->_texture_groups)
        if (d.frame_index > cached.last_used + texture_group_idle_frames)
            idle.push_back(key);
    for (auto const key : idle)
        (void)self->_texture_groups.erase(key);
    auto views = cc::vector<sg::named_view>();
    auto samplers = cc::vector<sg::named_sampler>();
    auto const bind_material = [&](auto& into, isize i)
    {
        auto const& draw = d.draws[i];
        into.bind_pipeline(*pipelines[draw.permutation]);
        into.bind_group(0, *group);
        into.bind_group(1, *tables);

        // The permutation actually bound, which is the stand-in while its own compiles, says whether there is a group 1.
        auto const* const material = bound[draw.permutation];
        auto const* const surface = self->_surfaces.get_ptr(material->key);
        if (surface == nullptr || surface->textures_layout == nullptr)
            return;
        auto const empty = cc::vector<sg::texture_2d>();
        auto const& textures = d.draw_textures.empty() ? empty : d.draw_textures[i];
        CC_ASSERT(textures.size() == material->bound_samplers.size(), "a textured draw binds one texture per "
                                                                      "texture slot of its permutation");
        auto key = cc::make_hash(surface->textures_layout.get(), material->key);
        for (auto const& t : textures)
            key = cc::combine_hash(key, cc::make_hash(t.raw().get()));
        auto& cached = self->_texture_groups[key];
        if (cached.group == nullptr)
        {
            views.clear();
            samplers.clear();
            for (auto k = isize(0); k < textures.size(); ++k)
            {
                // A slot the stage never reads is not in its layout, which names only what the compiler kept.
                if (k < surface->slot_bound.size() && !surface->slot_bound[k])
                    continue;
                views.push_back({.name = cc::format("sv_textures.t{}", k), .view = textures[k].as_texture_view()});
                samplers.push_back({.name = cc::format("sv_textures.s{}", k), .sampler = material->bound_samplers[k]});
            }
            cached.group = ctx.persistent.create_binding_group(surface->textures_layout, views, samplers);
        }
        cached.last_used = d.frame_index;
        into.bind_group(2, *cached.group);
    };

    // Every pixel is a surface's or the environment's, so neither color target is loaded.
    {
        auto pass = cmd.raster.render_to(bound_module::targets{
            .color = d.color.as_render_target_view().discarded(),
            .primary_depth = primary_depth.as_render_target_view().discarded(),
            .motion = motion.as_render_target_view().discarded(),
            .depth_stencil
            = prepass ? depth.as_depth_stencil_view().preserved() : depth.as_depth_stencil_view().cleared(0.0f),
        });
        if (!pulled)
            pass.bind_index_buffer(d.index_pool.as_index_buffer());
        for (auto i = isize(0); i < d.draws.size(); ++i)
        {
            auto const& draw = d.draws[i];
            CC_ASSERT(isize(draw.permutation) < pipelines.size(), "a raster draw names a permutation the frame holds");
            if (!drawn(draw) || is_quadric(draw))
                continue;
            bind_material(pass, i);
            issue(pass, draw);
        }

        // Last when no quadric follows, in the same scope.
        if (!any_quadric)
        {
            pass.bind_pipeline(*self->_environment);
            pass.bind_group(0, *group);
            pass.draw({.vertex_range = {.offset = 0, .size = 3}});
        }
    }

    // The quadric batches' boxes, in a scope of their own since their pixel stages write depth, then the environment
    // behind everything.
    if (any_quadric)
    {
        {
            auto quadric_pass = cmd.raster.render_to(bound_module::quadric_targets{
                .color = d.color.as_render_target_view().preserved(),
                .primary_depth = primary_depth.as_render_target_view().preserved(),
                .motion = motion.as_render_target_view().preserved(),
                .depth_stencil = depth.as_depth_stencil_view().preserved(),
            });
            for (auto i = isize(0); i < d.draws.size(); ++i)
            {
                auto const& draw = d.draws[i];
                if (!drawn(draw) || !is_quadric(draw))
                    continue;
                bind_material(quadric_pass, i);
                issue(quadric_pass, draw);
            }
        }
        auto environment_pass = cmd.raster.render_to(bound_module::targets{
            .color = d.color.as_render_target_view().preserved(),
            .primary_depth = primary_depth.as_render_target_view().preserved(),
            .motion = motion.as_render_target_view().preserved(),
            .depth_stencil = depth.as_depth_stencil_view().preserved(),
        });
        environment_pass.bind_pipeline(*self->_environment);
        environment_pass.bind_group(0, *group);
        environment_pass.draw({.vertex_range = {.offset = 0, .size = 3}});
    }

    if (d.stand_ins != nullptr)
        *d.stand_ins = stand_in_report::of(bound, substituted, pending);
    return sg::routine_outcome::executed;
}
} // namespace sv
