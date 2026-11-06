#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sgl_modules/raster_bound.hh>
#include <shaped-graphics/all.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/rendering/raster_bound_routine.hh>
#include <shaped-viewer/resources/material_shader_cache.hh>
#include <shaped-viewer/shader_library.hh>
#include <sv_shaders.hh>

namespace sv
{
namespace
{
namespace bound_module = sv::shaders::raster_bound;
namespace tracer = sv::shaders::tracer;

/// Reversed, `near / depth` cleared to 0 at infinity, as the bindless mode's.
constexpr auto depth_format = sg::pixel_format::depth32_float;

/// The entry every generated bound pixel stage declares.
constexpr auto bound_entry = cc::string_view("sv_raster_bound");

/// `raster_bound.targets` as a pipeline states it.
[[nodiscard]] bound_module::targets::states target_states()
{
    return {.color = {.format = sg::pixel_format::rgba32_float},
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

cc::shared_async<cc::unit> raster_bound_routine::init_once(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();
    _stand_ins = stand_ins{
        .lights = ctx.persistent.create_buffer_from_data(cc::vector<tracer::light_record>{tracer::light_record{}},
                                                         sg::buffer_usage::readonly_buffer),
        .bytes = ctx.persistent.create_buffer<byte>(16, sg::buffer_usage::readonly_buffer),
    };
    co_return;
}

cc::shared_async<cc::unit> raster_bound_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    // A reload re-generates every material's pixel stage against modules that may have moved.
    _surfaces.clear();
    _environment = nullptr;

    auto const surface_vs = shaders::raster_bound_pass::bound_vs->acquire(ctx);
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
        fail_init();
        co_return;
    }
    _environment = *built;
    co_return;
}

sg::raster_pipeline_handle raster_bound_routine::_surface_pipeline_of(sg::context& ctx, material_permutation const* p)
{
    auto& s = _surfaces.entry(p->key).get_or_emplace(surface_pipeline{});
    if (s.pipeline != nullptr)
        return s.pipeline;
    if (s.failed)
        return nullptr;

    if (!s.shader.is_valid())
    {
        CC_ASSERT(!p->bound_source.empty(), "every permutation a bound frame draws has a bound pixel stage");
        auto lib = acquire_shader_library();
        auto const format = lib.has_value() ? sgl_format_for(ctx, *lib.value()) : cc::optional<sg::shader_format>();
        if (!format.has_value())
        {
            CC_LOG_ERROR("no shader library builds {}'s bound raster stage into a format this context accepts", p->label);
            s.failed = true;
            return nullptr;
        }
        s.shader = lib.value()->compile_source(p->bound_source, sg::shader_stage::fragment, bound_entry, format.value(),
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

        // Two-sided like the tracer, which shades a triangle seen from behind through its turned normal.
        s.pending = ctx.cached.acquire_raster_pipeline({
            .layout = ctx.cached.acquire_pipeline_layout<bound_module::bound>(),
            .vertex_shader = _surface_vs,
            .fragment_shader = *compiled,
            .topology = sg::primitive_topology::triangle_list,
            .rasterization = {.cull = sg::cull_mode::none},
            .depth_stencil = {.depth_test = true, .depth_write = true, .depth_compare = sg::compare_op::greater_equal},
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
        CC_LOG_ERROR("the bound stage of {} did not build into a pipeline", p->label);
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
        // A quadric batch is not drawn yet, so its permutation is never compiled.
        if (p->kind == geometry_kind::quadrics)
        {
            pipelines.push_back(nullptr);
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
    auto const frame = ctx.transient.create_buffer_from_pod(cmd, d.frame, sg::buffer_usage::readonly_buffer);
    auto const group = ctx.transient.create_binding_group(
        cmd, ctx.cached.acquire_binding_group_layout<bound_module::bound>(),
        bound_module::bound{
            .frame = frame.as_readonly_buffer(),
            .instances = d.instance_table.as_readonly_buffer(),
            .lights = (d.lights.raw() != nullptr ? d.lights : self->_stand_ins.lights).as_readonly_buffer(),
            .background = d.background.reinterpret_as<tg::vec4f>().as_readonly_buffer(),
            .vertices = d.vertex_pool.reinterpret_as<byte>().as_readonly_buffer(),
            .indices = d.index_pool.reinterpret_as<byte>().as_readonly_buffer(),
            .params = bytes_or_stand_in(d.parameter_pool),
            .attributes = bytes_or_stand_in(d.attribute_pool),
        });

    // A layer with no drawings still has a primary depth written; this frame alone reads it, which is nobody.
    auto const primary_depth = d.primary_depth.raw() != nullptr
                                 ? d.primary_depth
                                 : ctx.transient.create_texture_2d({.format = sg::pixel_format::r32_float,
                                                                    .width = extent[0],
                                                                    .height = extent[1],
                                                                    .usage = sg::texture_usage::render_target});
    auto const depth = ctx.transient.create_texture_2d(
        {.format = depth_format, .width = extent[0], .height = extent[1], .usage = sg::texture_usage::depth_stencil});

    // Every pixel is a surface's or the environment's, so neither target is loaded.
    auto pass = cmd.raster.render_to(bound_module::targets{
        .color = d.color.as_render_target_view().discarded(),
        .primary_depth = primary_depth.as_render_target_view().discarded(),
        .depth_stencil = depth.as_depth_stencil_view().cleared(0.0f),
    });

    pass.bind_index_buffer(d.index_pool.as_index_buffer());
    for (auto const& draw : d.draws)
    {
        CC_ASSERT(isize(draw.permutation) < pipelines.size(), "a raster draw names a permutation the frame holds");
        // A frame mesh's own indices are not in the pool its pixel stage reads its corners from.
        if (draw.triangle_count == 0 || draw.indices.raw() != nullptr)
            continue;
        auto const& pipeline = pipelines[draw.permutation];
        if (pipeline == nullptr)
            continue;
        pass.bind_pipeline(*pipeline);
        pass.bind_group(0, *group);
        // The base vertex reaches the vertex stage in `@vertex_index`, so it reads the vertex pool at the index as it stands.
        pass.draw_indexed({.index_range = {.offset = isize(draw.first_index), .size = isize(draw.triangle_count) * 3},
                           .instance_range = {.offset = isize(draw.instance), .size = 1},
                           .vertex_offset = i32(draw.base_vertex)});
    }

    pass.bind_pipeline(*self->_environment);
    pass.bind_group(0, *group);
    pass.draw({.vertex_range = {.offset = 0, .size = 3}});

    if (d.stand_ins != nullptr)
        *d.stand_ins = stand_in_report::of(bound, substituted, pending);
    return sg::routine_outcome::executed;
}
} // namespace sv
