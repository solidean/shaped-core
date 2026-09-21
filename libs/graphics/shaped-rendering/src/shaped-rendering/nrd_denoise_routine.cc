#include <clean-core/common/assert.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <shaped-graphics/all.hh>
#include <shaped-rendering/impl/denoise_images.hh>
#include <shaped-rendering/impl/nrd_instance.hh>
#include <shaped-rendering/nrd_denoise_routine.hh>

#if SR_HAS_NRD
#include <shaped-rendering/impl/nrd_session.hh>
#include <sr_shaders.hh>
#endif

// Guarded as one block rather than split into two translation units, which is what the DLSS seam does.
// The difference is the shaders: NRD's repack pass includes NRD's own packing header, so without the SDK the package
// carries no `sr::shaders::nrd_repack` for a null implementation to name.

namespace sr
{
using impl::extent_of;
using impl::is_set;

nrd_options nrd_denoise_routine::options_for(denoise_settings const& settings)
{
    // Quality is how long a history may grow: a fast preset reacts sooner and stays noisier, a best one averages more
    // frames and smears a moving highlight further.
    switch (settings.quality)
    {
    case denoise_quality::fast:
        return {.max_accumulated_frames = 15, .max_fast_accumulated_frames = 4};
    case denoise_quality::balanced:
        return {.max_accumulated_frames = 30, .max_fast_accumulated_frames = 6};
    case denoise_quality::best:
        return {.max_accumulated_frames = 63, .max_fast_accumulated_frames = 8};
    }
    return {};
}

bool nrd_denoise_routine::is_available(sg::context const& ctx)
{
    (void)ctx;
    return impl::nrd_is_compiled_in();
}

#if SR_HAS_NRD
namespace
{
void release_session(void* p)
{
    delete static_cast<impl::nrd_session*>(p);
}

/// Which `history._state` slot holds what.
/// NRD reads and writes these every frame, but none of them is history: the temporal state lives inside the instance,
/// and these only exist per stream so a steady stream allocates nothing.
enum image_slot
{
    slot_normal_roughness = 0,
    slot_view_z = 1,
    slot_diffuse_in = 2,
    slot_specular_in = 3,
    slot_diffuse_out = 4,
    slot_specular_out = 5,
};
} // namespace

cc::shared_async<cc::unit> nrd_denoise_routine::init(sg::routine_init_scope scope)
{
    auto& ctx = scope.context();

    _repack_layout = nullptr;
    _resolve_layout = nullptr;
    _repack_pipeline = nullptr;
    _resolve_pipeline = nullptr;

    auto const repack_shader = sr::shaders::nrd_repack.compute.main_cs->acquire(ctx);
    auto const resolve_shader = sr::shaders::nrd_resolve.compute.main_cs->acquire(ctx);
    co_await cc::async_settled(repack_shader);
    co_await cc::async_settled(resolve_shader);

    auto const* const repack_compiled = repack_shader->try_value();
    auto const* const resolve_compiled = resolve_shader->try_value();
    if (repack_compiled == nullptr || resolve_compiled == nullptr)
    {
        fail_init();
        co_return;
    }

    auto const* const repack_constants = [&]() -> sg::binding const*
    {
        for (auto const& b : repack_compiled->bindings)
            if (b.type == sg::binding_type::uniform_buffer)
                return &b;
        return nullptr;
    }();
    if (repack_constants == nullptr)
    {
        fail_init(); // a shader that reflects no constants block cannot be driven
        co_return;
    }

    auto repack_layout = ctx.cached.acquire_binding_group_layout<shaders::nrd_repack_bindings>();
    auto resolve_layout = ctx.cached.acquire_binding_group_layout<shaders::nrd_resolve_bindings>();

    auto const repack_pipeline = ctx.cached.acquire_compute_pipeline(
        {.shader = *repack_compiled,
         .layout
         = ctx.cached.acquire_pipeline_layout({.groups = {repack_layout}, .inline_constants = *repack_constants})});
    auto const resolve_pipeline = ctx.cached.acquire_compute_pipeline(
        {.shader = *resolve_compiled, .layout = ctx.cached.acquire_pipeline_layout({.groups = {resolve_layout}})});

    co_await cc::async_settled(repack_pipeline);
    co_await cc::async_settled(resolve_pipeline);

    auto const* const repack_built = repack_pipeline->try_value();
    auto const* const resolve_built = resolve_pipeline->try_value();
    if (repack_built == nullptr || resolve_built == nullptr)
    {
        fail_init();
        co_return;
    }

    _repack_layout = cc::move(repack_layout);
    _resolve_layout = cc::move(resolve_layout);
    _repack_pipeline = *repack_built;
    _resolve_pipeline = *resolve_built;
    co_return;
}

denoise_outcome nrd_denoise_routine::execute(sg::command_list& cmd,
                                             denoise_inputs const& in,
                                             denoise_history& history,
                                             nrd_options const& options)
{
    CC_ASSERT(is_set(in.color), "a denoise call needs a colour texture");
    CC_ASSERT(is_set(in.output), "a denoise call needs an output texture");
    CC_ASSERT(extent_of(in.output) == extent_of(in.color), "NRD does not upscale: output and input extents differ");

    auto const outcome_of = [](denoise_status s, bool restarted = false)
    { return denoise_outcome{.status = s, .method = denoise_method::nrd, .restarted = restarted}; };

    // A guide this member requires but the call does not carry is `unsupported` rather than a degraded run: REBLUR
    // reprojects against every one of them, and a missing one is a wrong image with no diagnostic.
    if (!required_guides(denoise_method::nrd).without(in.present_guides()).is_empty())
        return outcome_of(denoise_status::unsupported);

    auto const self = try_acquire(cmd);
    if (self.is_pending())
        return outcome_of(denoise_status::pending);
    if (self.is_failed())
        return outcome_of(denoise_status::failed);

    auto& ctx = cmd.context();
    auto const extent = extent_of(in.color);
    auto const restarted = history._prepare(denoise_method::nrd, extent);

    // NRD's own inputs and outputs, at the input extent.
    // The normal-roughness format is the one NRD's build was compiled to read; `nrd_session::create` refuses a library
    // whose encoding says otherwise, so this pair cannot drift apart silently.
    (void)impl::ensure_image(ctx, history._state[slot_normal_roughness], extent, sg::pixel_format::rgb10a2_unorm);
    (void)impl::ensure_image(ctx, history._state[slot_view_z], extent, sg::pixel_format::r32_float);
    for (auto const slot : {slot_diffuse_in, slot_specular_in, slot_diffuse_out, slot_specular_out})
        (void)impl::ensure_image(ctx, history._state[slot], extent, sg::pixel_format::rgba16_float);

    if (history._vendor_state == nullptr)
    {
        // Owned raw because the history's slot is a `void*` with a release function beside it — the one shape that
        // lets `denoise.hh` free a vendor object whose type it must not name.
        auto* const fresh = new impl::nrd_session();
        if (!fresh->create(ctx, impl::nrd_denoiser::reblur_diffuse_specular, extent))
        {
            delete fresh;
            return outcome_of(denoise_status::failed);
        }

        history._vendor_state = fresh;
        history._release_vendor_state = &release_session;
    }

    auto& session = *static_cast<impl::nrd_session*>(history._vendor_state);

    // The instance exists well before its pipelines do — NRD's shaders build through the context's cache like ours.
    if (!session.is_ready())
        return outcome_of(denoise_status::pending, restarted);

    // Everything the tracer produces, in NRD's encodings.
    // The motion guide is NOT repacked: NRD reads ours untouched, with the sign and scale on its common settings.
    auto const repack_group = ctx.transient.create_binding_group(
        self->_repack_layout, shaders::nrd_repack_bindings{
                                  .gDiffuse = in.color.as_readonly_view(),
                                  .gSpecular = in.specular.as_readonly_view(),
                                  .gNormal = in.guides.normal.as_readonly_view(),
                                  .gRoughness = in.guides.roughness.as_readonly_view(),
                                  .gDepth = in.guides.depth.as_readonly_view(),
                                  .gHitDistance = in.guides.hit_distance.as_readonly_view(),
                                  .gNormalRoughness = history._state[slot_normal_roughness].as_readwrite_view(),
                                  .gViewZ = history._state[slot_view_z].as_readwrite_view(),
                                  .gDiffuseRadianceHitDistance = history._state[slot_diffuse_in].as_readwrite_view(),
                                  .gSpecularRadianceHitDistance = history._state[slot_specular_in].as_readwrite_view(),
                              });

    auto const hit_distance = impl::nrd_hit_distance_parameters();
    auto const repack_constants = shaders::nrd_repack_constants{
        .hit_distance_a = hit_distance[0],
        .hit_distance_b = hit_distance[1],
        .hit_distance_c = hit_distance[2],
        .sky_view_z = impl::nrd_sky_view_z(),
    };

    cmd.compute.bind_pipeline(*self->_repack_pipeline);
    cmd.compute.bind<shaders::nrd_repack_bindings>(*repack_group);
    cmd.compute.set_inline_constants(repack_constants);
    cmd.compute.dispatch_threads(extent[0], extent[1], 1);

    auto const frame = impl::nrd_frame{
        .world_to_view = in.guides.world_to_view,
        .view_to_clip = in.guides.view_to_clip,
        .previous_world_to_view = in.guides.previous_world_to_view,
        .previous_view_to_clip = in.guides.previous_view_to_clip,
        .jitter = in.guides.jitter,
        .previous_jitter = in.guides.previous_jitter,
        .frame_index = history._frame,
        .max_accumulated_frames = options.max_accumulated_frames,
        .max_fast_accumulated_frames = options.max_fast_accumulated_frames,
        .reset = restarted,
    };

    auto const resources = impl::nrd_resources{
        .motion = in.guides.motion,
        .normal_roughness = history._state[slot_normal_roughness],
        .view_z = history._state[slot_view_z],
        .diffuse_radiance_hit_distance = history._state[slot_diffuse_in],
        .specular_radiance_hit_distance = history._state[slot_specular_in],
        .out_diffuse_radiance_hit_distance = history._state[slot_diffuse_out],
        .out_specular_radiance_hit_distance = history._state[slot_specular_out],
    };

    if (!session.execute(cmd, frame, resources))
        return outcome_of(denoise_status::failed, restarted);

    auto const resolve_group = ctx.transient.create_binding_group(
        self->_resolve_layout, shaders::nrd_resolve_bindings{
                                   .gDiffuseRadianceHitDistance = history._state[slot_diffuse_out].as_readonly_view(),
                                   .gSpecularRadianceHitDistance = history._state[slot_specular_out].as_readonly_view(),
                                   .gOutput = in.output.as_readwrite_view(),
                               });

    cmd.compute.bind_pipeline(*self->_resolve_pipeline);
    cmd.compute.bind<shaders::nrd_resolve_bindings>(*resolve_group);
    cmd.compute.dispatch_threads(extent[0], extent[1], 1);

    ++history._frame;
    return outcome_of(denoise_status::denoised, restarted);
}

#else

cc::shared_async<cc::unit> nrd_denoise_routine::init(sg::routine_init_scope scope)
{
    (void)scope;
    co_return;
}

denoise_outcome nrd_denoise_routine::execute(sg::command_list& cmd,
                                             denoise_inputs const& in,
                                             denoise_history& history,
                                             nrd_options const& options)
{
    (void)cmd;
    (void)in;
    (void)history;
    (void)options;
    return {.status = denoise_status::unsupported, .method = denoise_method::nrd};
}

#endif
} // namespace sr
