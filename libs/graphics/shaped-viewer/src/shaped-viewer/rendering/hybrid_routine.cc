#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <sgl_modules/hybrid.hh>
#include <shaped-graphics/all.hh>
#include <shaped-viewer/impl/gpu_marks.hh>
#include <shaped-viewer/rendering/hybrid_routine.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh>
#include <shaped-viewer/resources/gpu_resource_manager.hh>
#include <sv_shaders.hh>

namespace sv
{
namespace tracer = sv::shaders::tracer;

cc::shared_async<cc::unit> hybrid_routine::init_once(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();
    _stand_ins = impl::traced_stand_ins::create(ctx);
    _output = ctx.persistent.create_texture_2d(
        {.format = sg::pixel_format::rgba32_float, .width = 1, .height = 1, .usage = sg::texture_usage::image});
    co_return;
}

cc::shared_async<cc::unit> hybrid_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();
    _light_thin = nullptr;
    _blend_guides = nullptr;
    if (!ctx.supports(sg::feature::ray_query) || !ctx.supports(sg::feature::raytracing_pipeline))
        co_return;
    auto const light_thin = shaders::hybrid_pass::light_thin.acquire_pipeline(ctx);
    auto const blend_guides = shaders::hybrid_pass::blend_guides.acquire_pipeline(ctx);
    co_await cc::async_settled(light_thin);
    co_await cc::async_settled(blend_guides);
    if (auto const* const built = light_thin->try_value(); built != nullptr)
        _light_thin = *built;
    else
        CC_LOG_ERROR("the hybrid's deferred lighting kernel did not build, so a deferred hybrid frame is declined: {}",
                     light_thin->try_error()->underlying().to_string());
    if (auto const* const built = blend_guides->try_value(); built != nullptr)
        _blend_guides = *built;
    else
        CC_LOG_ERROR("the hybrid's guide blend did not build, so a still hybrid frame is declined: {}",
                     blend_guides->try_error()->underlying().to_string());
    co_return;
}

sg::routine_outcome hybrid_routine::execute(sg::command_list& cmd, hybrid_frame_desc const& d)
{
    CC_RECORD_SCOPE("sv.hybrid");

    auto self = try_acquire(cmd);
    if (!self.is_ready())
        return sg::routine_outcome::declined;
    auto& ctx = cmd.context();
    auto t = d.targets;
    auto const deferred = d.lighting == hybrid_lighting::deferred;
    if (deferred && self->_light_thin == nullptr)
        return sg::routine_outcome::declined;

    CC_ASSERT(t.color.raw() != nullptr, "hybrid_routine: no color target bound");
    CC_ASSERT(d.bindless != nullptr, "hybrid_routine: a frame binds the manager's bindless tables");
    auto const extent = tg::vec2i(t.color.width(), t.color.height());

    // Every frame draws its pixels at another sub-pixel offset, so that the mean antialiases.
    // A still frame's guides written in place would describe that one offset, a different surface behind every edge each frame,
    // and a filter steered by them flickers there however converged the mean; so they blend as the tracer's do, on `guide_frame`.
    auto const blends = d.frame.guide_frame > 0;
    if (blends && self->_blend_guides == nullptr)
        return sg::routine_outcome::declined;
    auto const layer_guides = d.targets;
    auto const frame_guide = [&](sg::texture_2d const& like)
    {
        return ctx.transient.create_texture_2d(
            {.format = like.format(),
             .width = extent[0],
             .height = extent[1],
             .usage = sg::texture_usage::render_target | sg::texture_usage::image | sg::texture_usage::texture});
    };
    if (blends)
    {
        t.normal = frame_guide(layer_guides.normal);
        t.depth = frame_guide(layer_guides.depth);
        t.albedo = frame_guide(layer_guides.albedo);
        t.specular_albedo = frame_guide(layer_guides.specular_albedo);
        t.roughness = frame_guide(layer_guides.roughness);
    }
    auto const blend = [&]
    {
        if (!blends)
            return;
        auto const io = ctx.transient.create_binding_group(
            cmd, ctx.cached.acquire_binding_group_layout<shaders::hybrid_pass::guide_io>(),
            shaders::hybrid_pass::guide_io{
                .normal = t.normal.as_texture_view(),
                .depth = t.depth.as_texture_view(),
                .albedo = t.albedo.as_texture_view(),
                .specular_albedo = t.specular_albedo.as_texture_view(),
                .roughness = t.roughness.as_texture_view(),
                .blended_normal = layer_guides.normal.as_image_view<sg::pixel_format::rgba16_float>(),
                .blended_depth = layer_guides.depth.as_image_view<sg::pixel_format::r32_float>(),
                .blended_albedo = layer_guides.albedo.as_image_view<sg::pixel_format::rgba16_float>(),
                .blended_specular_albedo = layer_guides.specular_albedo.as_image_view<sg::pixel_format::rgba16_float>(),
                .blended_roughness = layer_guides.roughness.as_image_view<sg::pixel_format::r16_float>()});
        cmd.compute.bind_pipeline(*self->_blend_guides);
        cmd.compute.bind_group(0, *io);
        cmd.compute.set_inline_constants(shaders::hybrid_pass::guide_blend{.frames = d.frame.guide_frame}.to_block());
        cmd.compute.dispatch_threads(extent[0], extent[1]);
        impl::gpu_mark(cmd, "guide blend");
        if (d.frame_guides != nullptr)
            *d.frame_guides = t;
    };

    // The thin G-buffer is the frame's own: nothing outside it reads the reduced closure.
    auto const thin_target = [&](sg::pixel_format format)
    {
        return ctx.transient.create_texture_2d({.format = format,
                                                .width = extent[0],
                                                .height = extent[1],
                                                .usage = sg::texture_usage::render_target | sg::texture_usage::texture});
    };
    // A frame with no primary depth of its own, as one drawn for an upscaler, still writes one: the deferred kernel tells escapes by it.
    if (t.primary_depth.raw() == nullptr)
        t.primary_depth = thin_target(sg::pixel_format::r32_float);
    auto thin = raster_thin_targets{};
    if (deferred)
        thin = {.emission = thin_target(sg::pixel_format::rgba16_float),
                .normals = thin_target(sg::pixel_format::rgba16_float),
                .base = thin_target(sg::pixel_format::rgba8_unorm),
                .specular = thin_target(sg::pixel_format::rgba8_unorm),
                .extra = thin_target(sg::pixel_format::rgba8_unorm)};

    impl::gpu_mark(cmd, "before the hybrid");
    auto const tlas = cmd.raytracing.build_tlas(d.instances);
    impl::gpu_mark(cmd, "tlas");

    // A binding cannot be empty, so a frame with no lights binds one zeroed record that `light_count == 0` never reads.
    auto lights = d.lights;
    if (lights.raw() == nullptr)
    {
        lights = ctx.transient.create_buffer<tracer::light_record>(
            1, sg::buffer_usage::readonly_buffer | sg::buffer_usage::copy_dst);
        cmd.upload.pod_to_buffer(lights, tracer::light_record{});
    }
    auto const frame = ctx.transient.create_buffer_from_pod(cmd, d.frame, sg::buffer_usage::readonly_buffer);

    // A forward frame renders to the targets while `traced` is bound, so its images are all stand-ins.
    // A deferred frame's kernel writes the image and the guides the G-buffer does not hold through them instead.
    auto const trace = pt_trace_desc{
        .frame = frame,
        .background = d.background,
        .instances = d.instances,
        .output = deferred ? t.color : self->_output,
        .guide_normal = deferred ? t.normal : sg::texture_2d(),
        .guide_albedo = deferred ? t.albedo : sg::texture_2d(),
        .guide_specular_albedo = deferred ? t.specular_albedo : sg::texture_2d(),
        .guide_roughness = deferred ? t.roughness : sg::texture_2d(),
        .guide_hit_distance = deferred ? t.hit_distance : sg::texture_2d(),
        .instance_table = d.instance_table,
        .lights = lights,
        .emitters = d.emitters,
        .bindless = d.bindless,
        .decals = d.decals,
        .decal_shapes = d.decal_shapes,
        .decal_atlas = d.decal_atlas,
    };
    auto const traced = ctx.transient.create_binding_group(
        cmd, ctx.cached.acquire_binding_group_layout<tracer::traced>(),
        impl::traced_group_of(trace, self->_stand_ins, tlas, lights, frame, self->_stand_ins.paths));

    auto const outcome = raster_routine::execute(
        cmd, {.frame = {.camera = d.frame.camera,
                        .previous_camera = d.frame.previous_camera,
                        .near_plane = d.frame.near_plane,
                        .light_count = d.frame.light_count,
                        .decal_count = d.frame.decal_count,
                        .pixel_spread = d.frame.pixel_spread,
                        .pixel_width = d.frame.pixel_width,
                        .size = tg::vec2f(f32(extent[0]), f32(extent[1])),
                        .jitter = d.frame.fixed_jitter != 0 ? d.frame.jitter - tg::vec2f(0.5f, 0.5f) : tg::vec2f(0, 0),
                        // the forward stage's seed, so both lightings cut the same pixels on the same frame
                        .coverage_seed = d.frame.rng_seed + 1u},
              .background = d.background,
              .instance_table = d.instance_table,
              .lights = lights,
              .decals = d.decals,
              .decal_shapes = d.decal_shapes,
              .decal_atlas = d.decal_atlas,
              .draws = d.draws,
              .index_pool = d.index_pool,
              .permutations = d.permutations,
              .fallback = d.fallback,
              .bindless = d.bindless,
              .targets = t,
              .stand_ins = d.stand_ins,
              .visibility = d.visibility,
              .culling = d.culling,
              .shading = deferred ? raster_shading::thin : raster_shading::traced,
              .thin = thin,
              .traced = deferred ? sg::binding_group_handle() : traced});
    impl::gpu_mark(cmd, "raster");
    if (outcome != sg::routine_outcome::executed)
        return outcome;
    if (!deferred)
    {
        blend();
        return outcome;
    }

    auto const thin_io = ctx.transient.create_binding_group(
        cmd, ctx.cached.acquire_binding_group_layout<shaders::hybrid_pass::thin_io>(),
        shaders::hybrid_pass::thin_io{.emission = thin.emission.as_texture_view(),
                                      .normals = thin.normals.as_texture_view(),
                                      .base = thin.base.as_texture_view(),
                                      .specular = thin.specular.as_texture_view(),
                                      .extra = thin.extra.as_texture_view(),
                                      .depth = t.depth.as_texture_view(),
                                      .primary_depth = t.primary_depth.as_texture_view()});
    auto const declared = d.bindless->declare_compute_access(cmd);
    cmd.compute.bind_pipeline(*self->_light_thin);
    cmd.compute.bind_group(0, *traced);
    cmd.compute.bind_group(bindless_group, *d.bindless->group());
    cmd.compute.bind_group(2, *thin_io);
    cmd.compute.set_inline_constants(
        shaders::hybrid::pass{.size = tg::vec2f(f32(extent[0]), f32(extent[1])), .pad = tg::vec2f(0, 0)}.to_block());
    cmd.compute.dispatch_threads(extent[0], extent[1]);
    impl::gpu_mark(cmd, "deferred lighting");
    blend();
    return outcome;
}
} // namespace sv
