#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sgl_modules/hybrid.hh>
#include <sgl_modules/slug.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/slug_atlas.hh>
#include <shaped-shader-library/shader_library.hh>
#include <shaped-viewer/impl/gpu_marks.hh> // TEMPORARY(debt-sv-gpu-stage-marks)
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

/// A frame's depth buffer at `extent`, sampled too where a prepass's depth seeds the pyramid.
[[nodiscard]] sg::texture_2d depth_target(sg::context& ctx, tg::vec2i extent)
{
    return ctx.transient.create_texture_2d({.format = depth_format,
                                            .width = extent[0],
                                            .height = extent[1],
                                            .usage = sg::texture_usage::depth_stencil | sg::texture_usage::texture});
}

/// A frame's depth pyramid at `extent`: every level down to one texel, which occlusion culling builds and tests against.
[[nodiscard]] sg::texture_2d depth_pyramid(sg::context& ctx, tg::vec2i extent)
{
    auto levels = 1;
    while ((extent[0] >> levels) > 0 || (extent[1] >> levels) > 0)
        ++levels;
    return ctx.transient.create_texture_2d({.format = sg::pixel_format::r32_float,
                                            .width = extent[0],
                                            .height = extent[1],
                                            .mip_levels = levels,
                                            .usage = sg::texture_usage::texture | sg::texture_usage::image});
}

/// The faces of a quadric's box that are not drawn: the near ones, wound counter-clockwise from outside as `raster.box_face` winds them.
/// A camera inside the box sees only far faces, which is what the hybrid test placing one there holds this to.
constexpr auto quadric_box_cull = sg::cull_mode::front;

/// The seed of a surface pipeline's key, which hashes the permutation's key and the bindless layout it was built against.
constexpr u64 surface_key_seed = 0x5a5f3c;

/// The seed of a depth-only pipeline's key, which hashes the bindless layout it was built against and its vertex stage.
constexpr u64 depth_key_seed = 0x5a5f3d;

/// A group binds only at a pipeline built against its very layout object, so the manager's layout is part of the key.
[[nodiscard]] cc::hash128 surface_key_of(sv::material_permutation const& p, sv::bound_resources const& bindless)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(p.key);
    b.add_pod(bindless.layout().get());
    return cc::hash128::create(b.written_bytes(), surface_key_seed);
}

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

/// `raster.thin_targets` as a pipeline states it.
[[nodiscard]] raster::thin_targets::states thin_states()
{
    return {.emission = {.format = sg::pixel_format::rgba16_float},
            .normals = {.format = sg::pixel_format::rgba16_float},
            .base = {.format = sg::pixel_format::rgba8_unorm},
            .specular = {.format = sg::pixel_format::rgba8_unorm},
            .extra = {.format = sg::pixel_format::rgba8_unorm},
            .depth = {.format = sg::pixel_format::r32_float},
            .motion = {.format = sg::pixel_format::rg32_float},
            .primary_depth = {.format = sg::pixel_format::r32_float}};
}

/// The color targets a shading pipeline under `shading` writes.
[[nodiscard]] cc::fixed_vector<sg::color_target_state, sg::max_color_targets> color_targets_of(sv::raster_shading shading)
{
    if (shading == sv::raster_shading::thin)
        return thin_states();
    return target_states();
}

/// The layout every pipeline of a frame under `shading` shares, so the groups it binds fit each of them.
/// Group 1 is the manager's own layout, since a group fits only the very layout it was created against.
[[nodiscard]] sg::pipeline_layout_handle surface_layout_of(sg::context& ctx,
                                                           sv::bound_resources const& bindless,
                                                           sv::raster_shading shading)
{
    auto const traced = shading == sv::raster_shading::traced;
    auto groups = cc::small_vector<sg::binding_group_layout_handle, sg::max_binding_groups>();
    groups.push_back(traced ? ctx.cached.acquire_binding_group_layout<tracer::traced>()
                            : ctx.cached.acquire_binding_group_layout<raster::drawn>());
    groups.push_back(bindless.layout());
    groups.push_back(ctx.cached.acquire_binding_group_layout<sgl_modules::slug::tables>());
    if (traced)
        return ctx.cached.acquire_pipeline_layout(
            {.groups = cc::move(groups), .inline_constants = shaders::hybrid::pass::inline_binding()});
    return ctx.cached.acquire_pipeline_layout({.groups = cc::move(groups)});
}

/// The source of a permutation's pixel stage under `shading`.
[[nodiscard]] cc::string_view stage_source_of(sv::material_permutation const& p, sv::raster_shading shading)
{
    return shading == sv::raster_shading::traced ? cc::string_view(p.hybrid_source) : cc::string_view(p.raster_source);
}

/// The entry of a permutation's pixel stage under `shading`.
[[nodiscard]] cc::string_view stage_entry_of(sv::raster_shading shading)
{
    switch (shading)
    {
    case sv::raster_shading::lit:
        return "sv_raster";
    case sv::raster_shading::thin:
        return "sv_gbuffer";
    case sv::raster_shading::traced:
        return "sv_hybrid";
    }
    return "sv_raster";
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
    _depth_only.clear();
    _environment = nullptr;
    _environment_thin = nullptr;
    _hybrid_vs = {};
    _hybrid_quadric_vs = {};

    auto const surface_vs = shaders::raster_pass::surface_vs->acquire(ctx);
    auto const quadric_vs = shaders::raster_pass::quadric_vs->acquire(ctx);
    auto const environment_vs = shaders::raster_pass::environment_vs->acquire(ctx);
    auto const environment_ps = shaders::raster_pass::environment_ps->acquire(ctx);
    auto const environment_thin_ps = shaders::raster_pass::environment_thin_ps->acquire(ctx);
    co_await cc::async_settled(surface_vs);
    co_await cc::async_settled(quadric_vs);
    co_await cc::async_settled(environment_vs);
    co_await cc::async_settled(environment_ps);
    co_await cc::async_settled(environment_thin_ps);

    auto const* const compiled_surface_vs = surface_vs->try_value();
    auto const* const compiled_quadric_vs = quadric_vs->try_value();
    auto const* const compiled_environment_vs = environment_vs->try_value();
    auto const* const compiled_environment_ps = environment_ps->try_value();
    auto const* const compiled_environment_thin_ps = environment_thin_ps->try_value();
    if (compiled_surface_vs == nullptr || compiled_quadric_vs == nullptr || compiled_environment_vs == nullptr
        || compiled_environment_ps == nullptr || compiled_environment_thin_ps == nullptr)
    {
        fail_init();
        co_return;
    }
    _surface_vs = *compiled_surface_vs;
    _quadric_vs = *compiled_quadric_vs;

    // Behind every surface: drawn at depth 0, tested against the far plane the clear left, and writing no depth of its own.
    auto const environment_of = [&](sg::compiled_shader const& ps, raster_shading shading)
    {
        return ctx.cached.acquire_raster_pipeline({
            .layout = shaders::raster_pass::environment_ps.acquire_layout(ctx),
            .vertex_shader = *compiled_environment_vs,
            .fragment_shader = ps,
            .topology = sg::primitive_topology::triangle_list, // no vertex input: the vertex index
            .rasterization = {.cull = sg::cull_mode::none},
            .depth_stencil = {.depth_test = true, .depth_write = false, .depth_compare = sg::compare_op::greater_equal},
            .color_targets = color_targets_of(shading),
            .depth_stencil_format = depth_format,
        });
    };
    auto const environment = environment_of(*compiled_environment_ps, raster_shading::lit);
    auto const environment_thin = environment_of(*compiled_environment_thin_ps, raster_shading::thin);
    co_await cc::async_settled(environment);
    co_await cc::async_settled(environment_thin);
    auto const* const built = environment->try_value();
    auto const* const built_thin = environment_thin->try_value();
    if (built == nullptr || built_thin == nullptr)
    {
        fail_init();
        co_return;
    }
    _environment = *built;
    _environment_thin = *built_thin;

    // The forward hybrid's vertex stage exists only where the device traces, and a raster frame never waits on it.
    if (ctx.supports(sg::feature::ray_query) && ctx.supports(sg::feature::raytracing_pipeline))
    {
        auto const hybrid_vs = shaders::hybrid_pass::hybrid_vs->acquire(ctx);
        auto const hybrid_quadric_vs = shaders::hybrid_pass::hybrid_quadric_vs->acquire(ctx);
        co_await cc::async_settled(hybrid_vs);
        co_await cc::async_settled(hybrid_quadric_vs);
        auto const* const compiled_hybrid_vs = hybrid_vs->try_value();
        auto const* const compiled_hybrid_quadric_vs = hybrid_quadric_vs->try_value();
        if (compiled_hybrid_vs != nullptr && compiled_hybrid_quadric_vs != nullptr)
        {
            _hybrid_vs = *compiled_hybrid_vs;
            _hybrid_quadric_vs = *compiled_hybrid_quadric_vs;
        }
        else
            CC_LOG_ERROR("the hybrid's vertex stages did not compile, so a forward hybrid frame is declined");
    }

    // Culling is what makes a frame cheaper, never what makes it right, so a kernel that does not build draws everything.
    auto const cull = shaders::raster_cull::cull.acquire_pipeline(ctx);
    co_await cc::async_settled(cull);
    auto const* const cull_built = cull->try_value();
    _cull = cull_built != nullptr ? *cull_built : nullptr;
    if (_cull == nullptr)
        CC_LOG_ERROR("the raster cull kernel did not build, so a raster frame draws every item");

    _occlusion = {};
    auto const kernel = [&](auto const& entry) -> cc::shared_async<sg::compute_pipeline_handle>
    { return entry.acquire_pipeline(ctx); };
    auto const first = kernel(shaders::raster_cull::cull_first);
    auto const seed = kernel(shaders::raster_cull::pyramid_seed);
    auto const seed_depth = kernel(shaders::raster_cull::pyramid_seed_depth);
    auto const down = kernel(shaders::raster_cull::pyramid_down);
    auto const second = kernel(shaders::raster_cull::cull_second);
    co_await cc::async_settled(first);
    co_await cc::async_settled(seed);
    co_await cc::async_settled(seed_depth);
    co_await cc::async_settled(down);
    co_await cc::async_settled(second);
    auto const built_of = [](auto const& pending) -> sg::compute_pipeline_handle
    {
        auto const* const value = pending->try_value();
        return value != nullptr ? *value : nullptr;
    };
    auto kernels = occlusion_kernels{.cull_first = built_of(first),
                                     .pyramid_seed = built_of(seed),
                                     .pyramid_seed_depth = built_of(seed_depth),
                                     .pyramid_down = built_of(down),
                                     .cull_second = built_of(second)};
    if (kernels.cull_first != nullptr && kernels.pyramid_seed != nullptr && kernels.pyramid_seed_depth != nullptr
        && kernels.pyramid_down != nullptr && kernels.cull_second != nullptr)
        _occlusion = cc::move(kernels);
    else
        CC_LOG_ERROR("the raster occlusion kernels did not build, so a raster frame culls by the view alone");
    co_return;
}

sg::raster_pipeline_handle raster_routine::_surface_pipeline_of(sg::context& ctx,
                                                                material_permutation const* p,
                                                                bound_resources const& bindless,
                                                                raster_shading shading,
                                                                bool writes_depth)
{
    auto& entry = _surfaces.entry(surface_key_of(*p, bindless)).get_or_emplace(surface_pipeline{});
    auto& s = entry.stages[int(shading)];
    auto const variant = writes_depth ? 1 : 0;
    if (s.pipeline[variant] != nullptr)
        return s.pipeline[variant];
    if (s.failed)
        return nullptr;

    if (!s.shader.is_valid())
    {
        auto const source = stage_source_of(*p, shading);
        CC_ASSERT(!source.empty(), "every permutation a raster frame draws has a pixel stage");
        auto lib = acquire_shader_library();
        auto const format = lib.has_value() ? sgl_format_for(ctx, *lib.value()) : cc::optional<sg::shader_format>();
        if (!format.has_value())
        {
            CC_LOG_ERROR("no shader library builds {}'s raster stage into a format this context accepts", p->label);
            s.failed = true;
            return nullptr;
        }
        auto context_options = cc::vector<slib::shader_option>();
        slib::add_context_options(context_options, ctx);
        s.shader = lib.value()->compile_source(
            source, sg::shader_stage::fragment, stage_entry_of(shading), format.value(),
            {.language = slib::shader_language::sgl, .label = p->label, .options = context_options});
        (void)ctx.backlog.start(s.shader);
        return nullptr;
    }

    if (!s.pending[variant].is_valid())
    {
        if (!s.shader->is_ready())
            return nullptr;
        auto const* const compiled = s.shader->try_value();
        if (compiled == nullptr)
        {
            CC_LOG_ERROR("the {} stage of {} did not compile, so its geometry draws as the stand-in: {}",
                         stage_entry_of(shading), p->label, s.shader->try_error()->underlying().to_string());
            s.failed = true;
            return nullptr;
        }

        // Two-sided like the tracer, which shades a triangle seen from behind through its turned normal.
        // Both variants pass an equal depth, which is what lets a prepass's own depth through.
        // A quadric's box draws its far side alone, so a camera inside the box still covers it, and each pixel solves the surface once.
        auto const quadrics = p->kind == geometry_kind::quadrics;
        auto const traced = shading == raster_shading::traced;
        auto const vertex_shader = quadrics ? (traced ? _hybrid_quadric_vs.value() : _quadric_vs)
                                            : (traced ? _hybrid_vs.value() : _surface_vs);
        s.pending[variant] = ctx.cached.acquire_raster_pipeline({
            .layout = surface_layout_of(ctx, bindless, shading),
            .vertex_shader = vertex_shader,
            .fragment_shader = *compiled,
            .topology = sg::primitive_topology::triangle_list,
            .rasterization = {.cull = quadrics ? quadric_box_cull : sg::cull_mode::none},
            .depth_stencil
            = {.depth_test = true, .depth_write = writes_depth, .depth_compare = sg::compare_op::greater_equal},
            .color_targets = color_targets_of(shading),
            .depth_stencil_format = depth_format,
        });
        (void)ctx.backlog.start(s.pending[variant]);
        return nullptr;
    }

    if (!s.pending[variant]->is_ready())
        return nullptr;
    auto const* const built = s.pending[variant]->try_value();
    if (built == nullptr)
    {
        CC_LOG_ERROR("the {} stage of {} did not build into a pipeline", stage_entry_of(shading), p->label);
        s.failed = true;
        return nullptr;
    }
    s.pipeline[variant] = *built;
    return s.pipeline[variant];
}

raster_routine::depth_pipeline const& raster_routine::_depth_pipeline_of(sg::context& ctx,
                                                                         bound_resources const& bindless,
                                                                         bool traced)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(bindless.layout().get());
    b.add_pod(traced);
    auto& s = _depth_only.entry(cc::hash128::create(b.written_bytes(), depth_key_seed)).get_or_emplace(depth_pipeline{});
    if (s.pipeline != nullptr || s.failed)
        return s;

    if (!s.pending.is_valid())
    {
        // The vertex stage the shading draws run, so the depth it lays down is the very depth their test meets.
        s.pending = ctx.cached.acquire_raster_pipeline({
            .layout = surface_layout_of(ctx, bindless, traced ? raster_shading::traced : raster_shading::lit),
            .vertex_shader = traced ? _hybrid_vs.value() : _surface_vs,
            .topology = sg::primitive_topology::triangle_list,
            .rasterization = {.cull = sg::cull_mode::none},
            .depth_stencil = {.depth_test = true, .depth_write = true, .depth_compare = sg::compare_op::greater},
            .depth_stencil_format = depth_format,
        });
        (void)ctx.backlog.start(s.pending);
        return s;
    }

    if (!s.pending->is_ready())
        return s;
    auto const* const built = s.pending->try_value();
    if (built == nullptr)
    {
        CC_LOG_ERROR("the raster depth prepass did not build into a pipeline, so a raster frame shades its overdraw");
        s.failed = true;
        return s;
    }
    s.pipeline = *built;
    return s;
}

raster_scratch raster_routine::create_scratch(sg::context& ctx, tg::vec2i extent)
{
    return {.depth = depth_target(ctx, extent), .pyramid = depth_pyramid(ctx, extent)};
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
    auto const shading = d.shading;
    auto const traced = shading == raster_shading::traced;
    auto const thin = shading == raster_shading::thin;

    CC_ASSERT(thin || t.color.raw() != nullptr, "raster_routine: no color target bound");
    CC_ASSERT(thin || t.color.raw()->description().format == sg::pixel_format::rgba32_float,
              "raster_routine: the color target is rgba32_float, the layer's accumulation slot");
    CC_ASSERT(d.bindless != nullptr, "raster_routine: a frame binds the manager's bindless tables");
    CC_ASSERT(!traced || d.traced != nullptr, "raster_routine: a traced frame binds tracer.traced");
    CC_ASSERT(!thin || d.thin.emission.raw() != nullptr, "raster_routine: a thin frame binds the thin G-buffer");

    // A device that does not trace has no forward hybrid stage.
    if (traced && !self->_hybrid_vs.has_value())
        return sg::routine_outcome::declined;

    auto const extent = tg::vec2i(t.depth.width(), t.depth.height());

    // Declined while the prepass builds, so a frame that asks for one has it; one that failed shades its overdraw instead.
    auto depth_only = sg::raster_pipeline_handle();
    if (d.depth_prepass)
    {
        auto const& entry = self->_depth_pipeline_of(ctx, *d.bindless, traced);
        if (entry.pipeline == nullptr && !entry.failed)
            return sg::routine_outcome::declined;
        depth_only = entry.pipeline;
    }
    auto const prepass = depth_only != nullptr;

    // Started whether or not a substitution ends up needing them.
    auto const fallback_of = [&](bool writes_depth) -> sg::raster_pipeline_handle
    {
        return d.fallback != nullptr ? self->_surface_pipeline_of(ctx, d.fallback, *d.bindless, shading, writes_depth)
                                     : nullptr;
    };
    auto const fallback_writing = fallback_of(true);
    auto const fallback_tested = prepass ? fallback_of(false) : nullptr;

    // One bin per permutation, its draws contiguous in the argument buffers in the order the frame lists them.
    // The opaque bins come first, so a prepass draws all of them as one run.
    struct bin
    {
        sg::raster_pipeline_handle pipeline;
        bool cutout = false;
        /// a quadric permutation's, whose draws are boxes outside the argument buffers; no pipeline while it compiles
        bool quadrics = false;
        isize first = 0;
        isize count = 0;
    };
    auto bins = cc::vector<bin>();
    bins.reserve(d.permutations.size());
    auto bound = cc::vector<material_permutation const*>();
    bound.reserve(d.permutations.size());
    auto substituted = false;
    auto pending = false;
    for (auto const* p : d.permutations)
    {
        CC_ASSERT(p != nullptr, "a raster frame names a permutation the shader cache does not hold");
        // A quadric's depth is the surface its pixel solves, so it skips the prepass and writes it as it shades.
        // No stand-in draws its boxes while it compiles, since a triangle permutation's would read them as triangles.
        if (p->kind == geometry_kind::quadrics)
        {
            auto pipeline = self->_surface_pipeline_of(ctx, p, *d.bindless, shading, true);
            if (pipeline == nullptr)
            {
                auto const& stage = self->_surfaces.get_ptr(surface_key_of(*p, *d.bindless))->stages[int(shading)];
                pending = pending || !stage.failed;
                substituted = true;
            }
            bins.push_back({.pipeline = cc::move(pipeline), .quadrics = true});
            bound.push_back(p);
            continue;
        }
        // A cutout's depth depends on its material, so it skips the prepass and writes its depth as it shades.
        // The fallback standing in for one keeps that place.
        auto const cutout = p->opacity.kind != material_value_kind::none;
        auto const writes_depth = cutout || !prepass;
        auto pipeline = self->_surface_pipeline_of(ctx, p, *d.bindless, shading, writes_depth);
        if (pipeline == nullptr)
        {
            // _surface_pipeline_of made the entry, so it is there to ask whether the compile settled.
            auto const& stage = self->_surfaces.get_ptr(surface_key_of(*p, *d.bindless))->stages[int(shading)];
            pending = pending || !stage.failed;
            pipeline = writes_depth ? fallback_writing : fallback_tested;
            p = d.fallback;
            substituted = true;
        }
        if (pipeline == nullptr)
            return sg::routine_outcome::declined;
        bins.push_back({.pipeline = cc::move(pipeline), .cutout = cutout});
        bound.push_back(p);
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

    static_assert(sizeof(shaders::raster_cull::draw_args) == sizeof(sg::draw_indexed_args));
    // A frame mesh's draw binds its own index buffer, so it cannot join a bin's indirect run: it is drawn directly, never culled.
    auto own = cc::vector<raster_draw const*>();
    auto own_opaque = false;
    for (auto const& draw : d.draws)
    {
        CC_ASSERT(isize(draw.permutation) < bins.size(), "a raster draw names a permutation the frame holds");
        if (draw.triangle_count == 0)
            continue;
        if (draw.indices.raw() != nullptr)
        {
            own.push_back(&draw);
            own_opaque = own_opaque || !bins[draw.permutation].cutout;
        }
        else
            ++bins[draw.permutation].count;
    }
    auto total = isize(0);
    for (auto& b : bins)
        if (!b.cutout)
        {
            b.first = total;
            total += b.count;
        }
    auto const opaque_total = total;
    for (auto& b : bins)
        if (b.cutout)
        {
            b.first = total;
            total += b.count;
        }
    auto next = cc::vector<isize>();
    next.reserve(bins.size());
    for (auto const& b : bins)
        next.push_back(b.first);
    auto templates = cc::vector<shaders::raster_cull::draw_args>::create_defaulted(total);
    auto items = cc::vector<shaders::raster_cull::cull_item>::create_defaulted(total);
    for (auto const& draw : d.draws)
    {
        if (draw.triangle_count == 0 || draw.indices.raw() != nullptr)
            continue;
        auto const slot = next[draw.permutation]++;
        templates[slot] = {.index_count = draw.triangle_count * 3,
                           .instance_count = 1,
                           .first_index = draw.first_index,
                           .vertex_offset = i32(draw.base_vertex),
                           .first_instance = draw.instance};
        if (draw.bounds.has_value())
            items[slot] = {.box_min = draw.bounds.value().min - tg::pos3f::zero,
                           .has_box = 1,
                           .box_max = draw.bounds.value().max - tg::pos3f::zero,
                           .pad = 0};
    }

    // A layer with no drawings still has a primary depth written; this frame alone reads it, which is nobody.
    // Sampled too, since without a prepass the depth pyramid of an occlusion-culled frame is built from it.
    auto const primary_depth
        = t.primary_depth.raw() != nullptr
            ? t.primary_depth
            : ctx.transient.create_texture_2d({.format = sg::pixel_format::r32_float,
                                               .width = extent[0],
                                               .height = extent[1],
                                               .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
    auto const depth = d.scratch.depth.raw() != nullptr ? d.scratch.depth : depth_target(ctx, extent);

    auto const item_buffer = total > 0
                               ? ctx.transient.create_buffer_from_data(cmd, items, sg::buffer_usage::readonly_buffer)
                               : sg::buffer<shaders::raster_cull::cull_item>();
    auto const template_buffer = total > 0
                                   ? ctx.transient.create_buffer_from_data(
                                         cmd, templates,
                                         sg::buffer_usage::readonly_buffer | sg::buffer_usage::indirect_command_buffer
                                             | sg::buffer_usage::copy_src)
                                   : sg::buffer<shaders::raster_cull::draw_args>();
    auto const args_buffer = [&]
    {
        // Read-only too: the second pass reads what the first wrote.
        return ctx.transient.create_buffer<shaders::raster_cull::draw_args>(
            total, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::readonly_buffer
                       | sg::buffer_usage::indirect_command_buffer | sg::buffer_usage::copy_src);
    };
    auto const shape = shaders::raster_cull::raster_cull_shape{.count = u32(total), .pad = {}}.to_block();

    // Group 0 is the frame's scene: `tracer.traced` for a forward hybrid frame, which also takes the image's size inline.
    auto const pass_block
        = shaders::hybrid::pass{.size = tg::vec2f(f32(extent[0]), f32(extent[1])), .pad = tg::vec2f(0, 0)}.to_block();
    auto const bind_groups = [&](sg::rendering_scope& pass)
    {
        pass.bind_group(0, traced ? *d.traced : *drawn);
        pass.bind_group(bindless_group, *d.bindless->group());
        pass.bind_group(2, *tables);
        if (traced)
            pass.set_inline_constants(pass_block);
    };

    // A frame mesh's draw over its own index buffer, through `pipeline`, into the scope open on `pass`.
    auto const draw_own = [&](sg::rendering_scope& pass, raster_draw const& draw, sg::raster_pipeline const& pipeline)
    {
        pass.bind_index_buffer(draw.indices.as_index_buffer());
        pass.bind_pipeline(pipeline);
        bind_groups(pass);
        pass.draw_indexed({.index_range = {.offset = 0, .size = isize(draw.triangle_count) * 3},
                           .instance_range = {.offset = isize(draw.instance), .size = 1}});
    };

    // Every draw of every bin, from `args`, into the scope open on `pass`; with `with_own` the frame meshes' draws too.
    auto const draw_bins = [&](sg::rendering_scope& pass, sg::buffer<sg::draw_indexed_args> const& args, bool with_own)
    {
        if (total > 0)
        {
            CC_ASSERT(d.index_pool.raw() != nullptr, "raster_routine: a frame with draws binds the index pool");
            pass.bind_index_buffer(d.index_pool.as_index_buffer());
            for (auto const& b : bins)
            {
                if (b.count == 0)
                    continue;
                pass.bind_pipeline(*b.pipeline);
                bind_groups(pass);
                // The base vertex reaches the vertex stage in `@vertex_index`, so it reads the vertex pool at the index as it stands.
                pass.draw_indexed_indirect(args, b.first, b.count);
            }
        }
        if (with_own)
            for (auto const* draw : own)
                draw_own(pass, *draw, *bins[draw->permutation].pipeline);
    };

    // The depth of every opaque draw in `args`, into `depth` as `target` opens it; with `with_own` the opaque frame meshes' too.
    auto const draw_depth
        = [&](sg::buffer<sg::draw_indexed_args> const& args, sg::depth_stencil_target target, bool with_own)
    {
        auto pass = cmd.raster.render_to({.depth_stencil_target = cc::move(target)});
        if (opaque_total == 0 && !(with_own && own_opaque))
            return;
        // The vertex stage reads the vertex pool through the bindless tables.
        auto const declared = d.bindless->declare_raster_access(cmd);
        if (opaque_total > 0)
        {
            pass.bind_index_buffer(d.index_pool.as_index_buffer());
            pass.bind_pipeline(*depth_only);
            bind_groups(pass);
            pass.draw_indexed_indirect(args, 0, opaque_total);
        }
        if (with_own)
            for (auto const* draw : own)
                if (!bins[draw->permutation].cutout)
                    draw_own(pass, *draw, *depth_only);
    };

    // The scope the shading draws run in: every target, the depth as `depth_target` opens it.
    // Every pixel is a surface's or the environment's, so a target is loaded only where `keep` says an earlier pass drew it.
    // `far` clears the primary depth to 1, which a pyramid built from it reads before the environment fills it.
    auto const open_shading = [&](sg::depth_stencil_target depth_target, bool keep, bool far) -> sg::rendering_scope
    {
        auto const view = [&](sg::texture_2d const& texture)
        { return keep ? texture.as_render_target_view().preserved() : texture.as_render_target_view().discarded(); };
        auto const primary
            = far ? primary_depth.as_render_target_view().cleared(tg::vec4f(1, 1, 1, 1)) : view(primary_depth);
        if (thin)
            return cmd.raster.render_to(raster::thin_targets{
                .emission = view(d.thin.emission),
                .normals = view(d.thin.normals),
                .base = view(d.thin.base),
                .specular = view(d.thin.specular),
                .extra = view(d.thin.extra),
                .depth = view(t.depth),
                .motion = view(t.motion),
                .primary_depth = primary,
                .depth_stencil = cc::move(depth_target),
            });
        return cmd.raster.render_to(raster::targets{
            .color = view(t.color),
            .normal = view(t.normal),
            .depth = view(t.depth),
            .albedo = view(t.albedo),
            .specular_albedo = view(t.specular_albedo),
            .roughness = view(t.roughness),
            .motion = view(t.motion),
            .primary_depth = primary,
            .depth_stencil = cc::move(depth_target),
        });
    };

    // Every quadric batch's boxes, last, in a scope of their own: their pixel stages write depth, which is a target set of its own.
    // The prepass and the occlusion pyramid never saw them, and the environment the shading passes drew writes no depth, so they cover it.
    auto const draw_quadrics = [&]
    {
        auto any = false;
        for (auto const& draw : d.draws)
            any = any || (draw.primitive_count != 0 && bins[draw.permutation].pipeline != nullptr);
        if (!any)
            return;

        auto const keep = [](sg::texture_2d const& texture) { return texture.as_render_target_view().preserved(); };
        // A return per branch rather than `?:`: MSVC wants the deleted move for a conditional of two prvalue scopes, where a return elides it
        auto pass = [&]
        {
            if (thin)
                return cmd.raster.render_to(raster::quadric_thin_targets{
                    .emission = keep(d.thin.emission),
                    .normals = keep(d.thin.normals),
                    .base = keep(d.thin.base),
                    .specular = keep(d.thin.specular),
                    .extra = keep(d.thin.extra),
                    .depth = keep(t.depth),
                    .motion = keep(t.motion),
                    .primary_depth = keep(primary_depth),
                    .depth_stencil = depth.as_depth_stencil_view().preserved(),
                });
            return cmd.raster.render_to(raster::quadric_targets{
                .color = keep(t.color),
                .normal = keep(t.normal),
                .depth = keep(t.depth),
                .albedo = keep(t.albedo),
                .specular_albedo = keep(t.specular_albedo),
                .roughness = keep(t.roughness),
                .motion = keep(t.motion),
                .primary_depth = keep(primary_depth),
                .depth_stencil = depth.as_depth_stencil_view().preserved(),
            });
        }();
        auto const declared = d.bindless->declare_raster_access(cmd);
        for (auto const& draw : d.draws)
        {
            if (draw.primitive_count == 0)
                continue;
            auto const& b = bins[draw.permutation];
            if (b.pipeline == nullptr)
                continue;
            pass.bind_pipeline(*b.pipeline);
            bind_groups(pass);
            pass.draw({.vertex_range = {.offset = 0, .size = 36},
                       .instance_range = {.offset = isize(draw.instance), .size = isize(draw.primitive_count)}});
        }
    };

    auto const draw_environment = [&](sg::rendering_scope& pass)
    {
        pass.bind_pipeline(thin ? *self->_environment_thin : *self->_environment);
        pass.bind_group(0, *drawn);
        pass.draw({.vertex_range = {.offset = 0, .size = 3}});
    };

    // Two culling passes where the caller keeps a visibility record and the kernels built; otherwise one, culled by the view alone.
    auto const occlusion = d.culling == raster_culling::occlusion && d.visibility != nullptr && total > 0
                        && self->_cull != nullptr && self->_occlusion.has_value();
    auto drawn_args = sg::buffer<sg::draw_indexed_args>();
    if (!occlusion)
    {
        // none where every draw is a frame mesh's, drawn directly
        if (total > 0)
            drawn_args = template_buffer.reinterpret_as<sg::draw_indexed_args>();
        if (total > 0 && self->_cull != nullptr && d.culling != raster_culling::none)
        {
            auto const args = args_buffer();
            auto const group = ctx.transient.create_binding_group(
                cmd, ctx.cached.acquire_binding_group_layout<shaders::raster_cull::raster_cull_io>(),
                shaders::raster_cull::raster_cull_io{.frame = frame.as_readonly_buffer(),
                                                     .instances = d.instance_table.as_readonly_buffer(),
                                                     .items = item_buffer.as_readonly_buffer(),
                                                     .templates = template_buffer.as_readonly_buffer(),
                                                     .args = args.as_readwrite_buffer()});
            cmd.compute.bind_pipeline(*self->_cull);
            cmd.compute.bind_group(0, *group);
            cmd.compute.set_inline_constants(shape);
            cmd.compute.dispatch_threads(int(total));
            drawn_args = args.reinterpret_as<sg::draw_indexed_args>();
        }

        if (prepass)
            draw_depth(drawn_args, depth.as_depth_stencil_view().cleared(0.0f), true);

        // Without a prepass the depth is cleared here, to the far plane the environment tests against.
        auto pass = open_shading(
            prepass ? depth.as_depth_stencil_view().preserved() : depth.as_depth_stencil_view().cleared(0.0f), false,
            false);
        // Every table is declared once for the pass, after `pass` so it dies first.
        // The first draw applies them, and a later one finds them current unless its pipeline touches them at new stages.
        // The environment draw binds no table, so it leaves them dormant, and a pass with nothing else to draw ends them silently.
        auto const declared = d.bindless->declare_raster_access(cmd);
        draw_bins(pass, drawn_args, true);
        draw_environment(pass);
    }
    else
    {
        auto const& k = self->_occlusion.value();

        // A record of another frame's draws says nothing of these: everything starts not visible, and the second pass draws it.
        auto& history = *d.visibility;
        if (history.raw() == nullptr || history.element_count() != total)
        {
            history = ctx.persistent.create_buffer<u32>(
                total, sg::buffer_usage::readwrite_buffer | sg::buffer_usage::readonly_buffer
                           | sg::buffer_usage::copy_dst | sg::buffer_usage::copy_src);
            cmd.upload.data_to_buffer(history, cc::vector<u32>::create_defaulted(total));
        }

        // First: what was visible last frame and is in view.
        auto const first_args = args_buffer();
        {
            auto const group = ctx.transient.create_binding_group(
                cmd, ctx.cached.acquire_binding_group_layout<shaders::raster_cull::raster_first_io>(),
                shaders::raster_cull::raster_first_io{.frame = frame.as_readonly_buffer(),
                                                      .instances = d.instance_table.as_readonly_buffer(),
                                                      .items = item_buffer.as_readonly_buffer(),
                                                      .templates = template_buffer.as_readonly_buffer(),
                                                      .history = history.as_readonly_buffer(),
                                                      .args = first_args.as_readwrite_buffer()});
            cmd.compute.bind_pipeline(*k.cull_first);
            cmd.compute.bind_group(0, *group);
            cmd.compute.set_inline_constants(shape);
            cmd.compute.dispatch_threads(int(total));
        }
        auto const first_drawn = first_args.reinterpret_as<sg::draw_indexed_args>();
        // The frame meshes are drawn with the first pass, so they occlude in the pyramid and are drawn once.
        if (prepass)
            draw_depth(first_drawn, depth.as_depth_stencil_view().cleared(0.0f), true);
        else
        {
            auto pass = open_shading(depth.as_depth_stencil_view().cleared(0.0f), false, true);
            auto const declared = d.bindless->declare_raster_access(cmd);
            draw_bins(pass, first_drawn, true);
        }
        impl::gpu_mark(cmd, "raster: first cull and draw");

        // The pyramid: the farthest primary depth under each texel, level by level.
        auto const pyramid = d.scratch.pyramid.raw() != nullptr ? d.scratch.pyramid : depth_pyramid(ctx, extent);
        auto const levels = pyramid.mip_levels();
        auto const pyramid_layout = ctx.cached.acquire_binding_group_layout<shaders::raster_cull::pyramid_io>();
        auto const level_group = [&](sg::raw_view const& source, int level)
        {
            return ctx.transient.create_binding_group(
                cmd, pyramid_layout,
                shaders::raster_cull::pyramid_io{
                    .source = sg::as_texture_view(source).as_texture<sg::tv_2d>(),
                    .target = pyramid.as_image_view<sg::pixel_format::r32_float>({.mip = level})});
        };
        if (prepass)
        {
            auto const group = ctx.transient.create_binding_group(
                cmd, ctx.cached.acquire_binding_group_layout<shaders::raster_cull::pyramid_depth_io>(),
                shaders::raster_cull::pyramid_depth_io{
                    .source = depth.as_texture_view(),
                    .target = pyramid.as_image_view<sg::pixel_format::r32_float>({.mip = 0})});
            cmd.compute.bind_pipeline(*k.pyramid_seed_depth);
            cmd.compute.bind_group(0, *group);
        }
        else
        {
            cmd.compute.bind_pipeline(*k.pyramid_seed);
            cmd.compute.bind_group(0, *level_group(primary_depth.as_texture_view(), 0));
        }
        cmd.compute.dispatch_threads(extent[0], extent[1]);
        for (auto level = 1; level < levels; ++level)
        {
            cmd.compute.bind_pipeline(*k.pyramid_down);
            cmd.compute.bind_group(
                0, *level_group(pyramid.as_texture_view({.mips = {.start = level - 1, .count = 1}}), level));
            auto const width = extent[0] >> level;
            auto const height = extent[1] >> level;
            cmd.compute.dispatch_threads(width > 0 ? width : 1, height > 0 ? height : 1);
        }
        impl::gpu_mark(cmd, "raster: depth pyramid");

        // Second: everything tested against the pyramid, drawing what is visible and was not drawn first, and recording what is visible.
        auto const second_args = args_buffer();
        {
            auto const group = ctx.transient.create_binding_group(
                cmd, ctx.cached.acquire_binding_group_layout<shaders::raster_cull::raster_second_io>(),
                shaders::raster_cull::raster_second_io{.frame = frame.as_readonly_buffer(),
                                                       .instances = d.instance_table.as_readonly_buffer(),
                                                       .items = item_buffer.as_readonly_buffer(),
                                                       .templates = template_buffer.as_readonly_buffer(),
                                                       .first = first_args.as_readonly_buffer(),
                                                       .pyramid = pyramid.as_texture_view(),
                                                       .history = history.as_readwrite_buffer(),
                                                       .args = second_args.as_readwrite_buffer()});
            cmd.compute.bind_pipeline(*k.cull_second);
            cmd.compute.bind_group(0, *group);
            cmd.compute.set_inline_constants(shaders::raster_cull::raster_second_shape{
                .count = u32(total),
                .levels = u32(levels),
                .pad = {}}.to_block());
            cmd.compute.dispatch_threads(int(total));
        }
        drawn_args = second_args.reinterpret_as<sg::draw_indexed_args>();
        impl::gpu_mark(cmd, "raster: second cull");

        if (prepass)
        {
            // Every surface is shaded once, both passes' draws against the depth they laid down together.
            draw_depth(drawn_args, depth.as_depth_stencil_view().preserved(), false);
            auto pass = open_shading(depth.as_depth_stencil_view().preserved(), false, false);
            auto const declared = d.bindless->declare_raster_access(cmd);
            draw_bins(pass, first_drawn, true);
            draw_bins(pass, drawn_args, false);
            draw_environment(pass);
        }
        else
        {
            // The first pass's surfaces stay, and the environment fills what neither pass covered.
            auto pass = open_shading(depth.as_depth_stencil_view().preserved(), true, false);
            auto const declared = d.bindless->declare_raster_access(cmd);
            draw_bins(pass, drawn_args, false);
            draw_environment(pass);
        }
        impl::gpu_mark(cmd, "raster: depth and shading");
    }
    draw_quadrics();
    if (d.drawn_args != nullptr)
        *d.drawn_args = drawn_args;

    if (d.stand_ins != nullptr)
        *d.stand_ins = stand_in_report::of(bound, substituted, pending);
    return sg::routine_outcome::executed;
}
} // namespace sv
