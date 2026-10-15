#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sgl_modules/slug.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/rendering/raster_routine.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <shaped-viewer/resources/material_shader_cache.hh>
#include <shaped-viewer/shader_library.hh>
#include <shaped-viewer/view/view_data.hh>
#include <sv_shaders.hh>

namespace sv
{
namespace
{
namespace raster = sv::shaders::raster;
namespace tracer = sv::shaders::tracer;

/// The depth a raster frame tests its surfaces against, which nothing outside the frame reads.
/// It is reversed — `near / depth`, cleared to 0 at infinity — so a float resolves a far surface as finely as a near one.
constexpr auto depth_format = sg::pixel_format::depth32_float;

/// `raster.targets` as a pipeline states it: each target in the format its slot is allocated in.
[[nodiscard]] raster::targets::states target_states()
{
    return {.color = {.format = sg::pixel_format::rgba32_float},
            .normal = {.format = sg::pixel_format::rgba16_float},
            .depth = {.format = sg::pixel_format::r32_float},
            .albedo = {.format = sg::pixel_format::rgba16_float},
            .specular_albedo = {.format = sg::pixel_format::rgba16_float},
            .roughness = {.format = sg::pixel_format::r16_float},
            .motion = {.format = sg::pixel_format::rg32_float},
            .primary_depth = {.format = sg::pixel_format::r32_float}};
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

cc::shared_async<cc::unit> raster_routine::init_once(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // `view_renderer` binds the slots of `impl::raster_slots` as these targets, so each must be allocated in the format its target is drawn in.
    struct bound_target
    {
        u64 (*id)(u8 layer) = nullptr;
        sg::pixel_format format = sg::pixel_format::undefined;
    };
    auto const states = target_states();
    bound_target const bound[] = {
        {.id = temporal_id::normal_guide, .format = states.normal.format},
        {.id = temporal_id::depth_guide, .format = states.depth.format},
        {.id = temporal_id::albedo_guide, .format = states.albedo.format},
        {.id = temporal_id::specular_albedo_guide, .format = states.specular_albedo.format},
        {.id = temporal_id::roughness_guide, .format = states.roughness.format},
        {.id = temporal_id::motion_guide, .format = states.motion.format},
    };
    CC_ASSERT(cc::span<bound_target const>(bound).size() == cc::span<impl::raster_slot const>(impl::raster_slots).size(),
              "every raster slot is one target of the frame");
    for (auto const& t : bound)
    {
        auto found = false;
        for (auto const& s : impl::raster_slots)
            if (s.id == t.id)
            {
                CC_ASSERT(s.format == t.format, "a raster slot is allocated in the format its target is drawn in");
                found = true;
            }
        CC_ASSERT(found, "every G-buffer target of a raster frame is one of impl::raster_slots");
    }
    auto const table = [&](sg::pixel_format format)
    {
        return ctx.persistent.create_texture_2d(
            {.format = format, .width = 1, .height = 1, .usage = sg::texture_usage::texture});
    };
    _stand_ins = stand_ins{
        .lights = ctx.persistent.create_buffer_from_data(cc::vector<tracer::light_record>{tracer::light_record{}},
                                                         sg::buffer_usage::readonly_buffer),
        .decals = ctx.persistent.create_buffer<tracer::decal_record>(1, sg::buffer_usage::readonly_buffer),
        .decal_shapes = ctx.persistent.create_buffer<tracer::decal_shape>(1, sg::buffer_usage::readonly_buffer),
        .decal_curves = table(sg::pixel_format::rgba16_float),
        .decal_bands = table(sg::pixel_format::rg16_uint),
    };
    co_return;
}

cc::shared_async<cc::unit> raster_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // A reload re-generates every material's pixel stage against modules that may have moved.
    _surfaces.clear();
    _environment = nullptr;

    auto const surface_vs = shaders::raster_pass.surface_vs->acquire(ctx);
    auto const environment_vs = shaders::raster_pass.environment_vs->acquire(ctx);
    auto const environment_ps = shaders::raster_pass.environment_ps->acquire(ctx);
    co_await cc::async_settled(surface_vs);
    co_await cc::async_settled(environment_vs);
    co_await cc::async_settled(environment_ps);

    auto const* const compiled_surface_vs = surface_vs->try_value();
    auto const* const compiled_environment_vs = environment_vs->try_value();
    auto const* const compiled_environment_ps = environment_ps->try_value();
    if (compiled_surface_vs == nullptr || compiled_environment_vs == nullptr || compiled_environment_ps == nullptr)
    {
        fail_init();
        co_return;
    }
    _surface_vs = *compiled_surface_vs;

    // Behind every surface: drawn at depth 0, tested against the far plane the clear left, and writing no depth of its own.
    auto const environment = ctx.cached.acquire_raster_pipeline({
        .layout = shaders::raster_pass.environment_ps.acquire_layout(ctx),
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
        fail_init();
        co_return;
    }
    _environment = *built;
    co_return;
}

sg::raster_pipeline_handle raster_routine::_surface_pipeline_of(sg::context& ctx,
                                                                material_permutation const* p,
                                                                bound_resources const& bindless)
{
    auto& s = _surfaces.entry(p->key).get_or_emplace(surface_pipeline{});
    if (s.pipeline != nullptr)
        return s.pipeline;
    if (s.failed)
        return nullptr;

    if (!s.shader.is_valid())
    {
        CC_ASSERT(!p->raster_source.empty(), "a raster frame draws a triangle permutation, which has a pixel stage");
        auto lib = acquire_shader_library();
        auto const format = lib.has_value() ? sgl_format_for(ctx, *lib.value()) : cc::optional<sg::shader_format>();
        if (!format.has_value())
        {
            CC_LOG_ERROR("no shader library builds {}'s raster stage into a format this context accepts", p->label);
            s.failed = true;
            return nullptr;
        }
        s.shader = lib.value()->compile_source(p->raster_source, sg::shader_stage::fragment, "sv_raster", format.value(),
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
            CC_LOG_ERROR("the raster stage of {} did not compile, so its geometry draws as the stand-in: {}", p->label,
                         s.shader->try_error()->underlying().to_string());
            s.failed = true;
            return nullptr;
        }

        // Group 1 is the manager's own layout, since a group fits only the very layout it was created against.
        auto groups = cc::small_vector<sg::binding_group_layout_handle, sg::max_binding_groups>();
        groups.push_back(ctx.cached.acquire_binding_group_layout<raster::drawn>());
        groups.push_back(bindless.layout());
        groups.push_back(ctx.cached.acquire_binding_group_layout<sgl_modules::slug::tables>());
        auto const layout = ctx.cached.acquire_pipeline_layout(
            {.groups = cc::move(groups), .inline_constants = raster::draw::inline_binding()});

        // Two-sided like the tracer, which shades a triangle seen from behind through its turned normal.
        s.pending = ctx.cached.acquire_raster_pipeline({
            .layout = layout,
            .vertex_shader = _surface_vs,
            .fragment_shader = *compiled,
            .topology = sg::primitive_topology::triangle_list,
            .rasterization = {.cull = sg::cull_mode::none},
            .depth_stencil = {.depth_test = true, .depth_write = true, .depth_compare = sg::compare_op::greater},
            .color_targets = target_states(),
            .depth_stencil_format = depth_format,
        });
        (void)ctx.backlog.start(s.pending);
        return nullptr;
    }

    if (!s.pending->is_ready())
        return nullptr;
    auto const* const built = s.pending->try_value();
    if (built == nullptr)
    {
        CC_LOG_ERROR("the raster stage of {} did not build into a pipeline", p->label);
        s.failed = true;
        return nullptr;
    }
    s.pipeline = *built;
    return s.pipeline;
}

sg::routine_outcome raster_routine::execute(sg::command_list& cmd, raster_frame_desc const& d)
{
    CC_RECORD_SCOPE("sv.raster");

    // Exclusive: _surface_pipeline_of writes the pipeline map.
    auto self = try_acquire_exclusive(cmd);
    if (!self.is_ready() || self->_environment == nullptr)
        return sg::routine_outcome::declined;
    auto& ctx = cmd.context();
    auto const& t = d.targets;

    CC_ASSERT(t.color.raw() != nullptr, "raster_routine: no color target bound");
    CC_ASSERT(t.color.raw()->description().format == sg::pixel_format::rgba32_float,
              "raster_routine: the color target is rgba32_float, the layer's accumulation slot");
    CC_ASSERT(d.bindless != nullptr, "raster_routine: a frame binds the manager's bindless tables");

    auto const extent = tg::vec2i(t.color.width(), t.color.height());

    // Started whether or not a substitution ends up needing it.
    auto const fallback = d.fallback != nullptr ? self->_surface_pipeline_of(ctx, d.fallback, *d.bindless) : nullptr;

    auto pipelines = cc::vector<sg::raster_pipeline_handle>();
    pipelines.reserve(d.permutations.size());
    auto substituted = false;
    for (auto const* p : d.permutations)
    {
        CC_ASSERT(p != nullptr, "a raster frame names a permutation the shader cache does not hold");
        CC_ASSERT(p->kind == geometry_kind::triangles, "a raster frame draws triangle permutations alone");
        auto pipeline = self->_surface_pipeline_of(ctx, p, *d.bindless);
        if (pipeline == nullptr)
        {
            pipeline = fallback;
            substituted = true;
        }
        if (pipeline == nullptr)
            return sg::routine_outcome::declined;
        pipelines.push_back(cc::move(pipeline));
    }

    auto const frame = ctx.transient.create_buffer_from_pod(cmd, d.frame, sg::buffer_usage::readonly_buffer);
    auto const drawn = ctx.transient.create_binding_group(
        cmd, ctx.cached.acquire_binding_group_layout<raster::drawn>(),
        raster::drawn{
            .frame = frame.as_readonly_buffer(),
            .instances = d.instance_table.as_readonly_buffer(),
            .lights = (d.lights.raw() != nullptr ? d.lights : self->_stand_ins.lights).as_readonly_buffer(),
            .background = d.background.reinterpret_as<tg::vec4f>().as_readonly_buffer(),
            .decals = (d.decals.raw() != nullptr ? d.decals : self->_stand_ins.decals).as_readonly_buffer(),
            .decal_shapes
            = (d.decal_shapes.raw() != nullptr ? d.decal_shapes : self->_stand_ins.decal_shapes).as_readonly_buffer(),
        });

    // An atlas nothing was placed in has no textures yet, so the stand-ins serve it as they serve no atlas at all.
    auto const has_atlas = d.decal_atlas != nullptr && d.decal_atlas->curve_texture().raw() != nullptr
                        && d.decal_atlas->band_texture().raw() != nullptr;
    auto const tables = ctx.transient.create_binding_group(
        cmd, ctx.cached.acquire_binding_group_layout<sgl_modules::slug::tables>(),
        sgl_modules::slug::tables{
            .curves = (has_atlas ? d.decal_atlas->curve_texture() : self->_stand_ins.decal_curves).as_texture_view(),
            .bands = (has_atlas ? d.decal_atlas->band_texture() : self->_stand_ins.decal_bands).as_texture_view()});

    // A layer with no drawings still has a primary depth written; this frame alone reads it, which is nobody.
    auto const primary_depth = t.primary_depth.raw() != nullptr
                                 ? t.primary_depth
                                 : ctx.transient.create_texture_2d({.format = sg::pixel_format::r32_float,
                                                                    .width = extent[0],
                                                                    .height = extent[1],
                                                                    .usage = sg::texture_usage::render_target});
    auto const depth = ctx.transient.create_texture_2d(
        {.format = depth_format, .width = extent[0], .height = extent[1], .usage = sg::texture_usage::depth_stencil});

    // Every pixel of every target is written, by a surface or by the environment behind them, so nothing is loaded.
    // The depth alone is cleared, to the far plane the environment tests against.
    auto pass = cmd.raster.render_to(raster::targets{
        .color = t.color.as_render_target_view().discarded(),
        .normal = t.normal.as_render_target_view().discarded(),
        .depth = t.depth.as_render_target_view().discarded(),
        .albedo = t.albedo.as_render_target_view().discarded(),
        .specular_albedo = t.specular_albedo.as_render_target_view().discarded(),
        .roughness = t.roughness.as_render_target_view().discarded(),
        .motion = t.motion.as_render_target_view().discarded(),
        .primary_depth = primary_depth.as_render_target_view().discarded(),
        .depth_stencil = depth.as_depth_stencil_view().cleared(0.0f),
    });

    // Every table is declared once for the pass, after `pass` so it dies first.
    // The first draw applies them, and a later one finds them current unless its pipeline touches them at new stages.
    // The environment draw binds no table, so it leaves them dormant, and a pass with nothing else to draw ends them silently.
    auto const declared = d.bindless->declare_raster_access(cmd);
    for (auto const& draw : d.draws)
    {
        CC_ASSERT(isize(draw.permutation) < pipelines.size(), "a raster draw names a permutation the frame does not "
                                                              "hold");
        if (draw.triangle_count == 0)
            continue;
        pass.bind_pipeline(*pipelines[draw.permutation]);
        pass.bind_group(0, *drawn);
        pass.bind_group(bindless_group, *d.bindless->group());
        pass.bind_group(2, *tables);
        pass.set_inline_constants(raster::draw{.instance = draw.instance, .pad0 = {}}.to_block());
        pass.draw({.vertex_range = {.offset = 0, .size = isize(draw.triangle_count) * 3}});
    }

    pass.bind_pipeline(*self->_environment);
    pass.bind_group(0, *drawn);
    pass.draw({.vertex_range = {.offset = 0, .size = 3}});

    if (d.drawn_with_stand_ins != nullptr)
        *d.drawn_with_stand_ins = substituted;
    return sg::routine_outcome::executed;
}
} // namespace sv
