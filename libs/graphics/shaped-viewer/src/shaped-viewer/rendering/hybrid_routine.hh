#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/thread/async.hh>
#include <sgl_modules/tracer.hh> // the records a hybrid frame shares with the tracer
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/rendering/raster_routine.hh>
#include <shaped-viewer/rendering/traced_group.hh>
#include <shaped-viewer/scene/background.hh>
#include <shaped-viewer/view/render_settings.hh> // sv::hybrid_lighting

/// Everything one hybrid frame binds and draws: a raster frame's draws, and the trace's scene beside them.
struct sv::hybrid_frame_desc
{
    hybrid_lighting lighting = hybrid_lighting::forward;

    /// The tracer's frame block, as a trace of the layer would fill it: the camera, the previous camera, the near plane, the light table and the decal count.
    /// `rng_seed` seeds each pixel's samples, so a caller averaging frames changes it per frame.
    shaders::tracer::frame_constants frame = {};

    /// The SH environment probe, which lights every surface and fills every pixel no surface covers.
    sg::buffer<background_gpu> background;

    /// One TLAS instance per row of `instance_table`, in its order, which the frame's TLAS is built from.
    cc::span<sg::tlas_instance const> instances;

    /// The TLAS of `instances`, built by a caller drawing several samples of one scene in one frame; null builds one.
    sg::tlas_handle tlas;

    /// One record per row, as the tracer's instance table holds them; `raster_draw::instance` indexes it.
    sg::buffer<shaders::tracer::instance_record> instance_table;

    /// Every light, grouped by path as `pt_light_table` groups them, and the emitters among them; null for none.
    sg::buffer<shaders::tracer::light_record> lights;
    sg::buffer<shaders::tracer::emitter_record> emitters;

    /// The decals, the shapes they cover, and the atlas those live in; null for none.
    sg::buffer<shaders::tracer::decal_record> decals;
    sg::buffer<shaders::tracer::decal_shape> decal_shapes;
    sr::slug_atlas const* decal_atlas = nullptr;

    /// The draws and what they shade with, as `raster_frame_desc` holds them.
    cc::span<raster_draw const> draws;
    sg::buffer<u32> index_pool;
    cc::span<material_permutation const* const> permutations;
    material_permutation const* fallback = nullptr;
    bound_resources const* bindless = nullptr;

    /// Where the frame is written, every target at one extent: `color` is this frame's lit image, one sample per pixel.
    /// The rest are its guides, as a raster frame writes them; every texture also needs `image` usage, which a deferred frame writes them through.
    raster_gbuffer targets;

    /// A deferred frame's thin G-buffer, from `hybrid_routine::create_thin_targets` at the frame's extent; empty creates one.
    /// A caller drawing several samples in one frame passes one, so they share it: each is about 28 bytes a pixel, which a pack would multiply.
    raster_thin_targets thin;

    /// The raster pass's depth buffer and pyramid, shared by a pack as `thin` is; empty creates them.
    raster_scratch scratch;

    /// Where set, receives this frame's own guides on a frame whose `frame.guide_frame` blends them into `targets`, and nothing otherwise.
    /// A temporal denoiser of this frame's samples reads them, since the blended ones describe other offsets than its samples'.
    raster_gbuffer* frame_guides = nullptr;

    /// What `raster_frame_desc` says of each.
    stand_in_report* stand_ins = nullptr;
    sg::buffer<u32>* visibility = nullptr;
    raster_culling culling = raster_culling::occlusion;
};

/// The hybrid: rasterized primary hits, lit by the tracer's next-event estimate with inline shadow rays.
///
/// One light sample and one environment sample per pixel and frame, each with its shadow ray, so a frame is noisy and its mean is the tracer's direct light.
/// Every frame builds its TLAS from `hybrid_frame_desc::instances` and binds `tracer.traced` over it.
///
/// - **Forward** draws through `raster_routine` with `raster_shading::traced`: each material's pixel stage lights its own full closure, after a depth prepass.
/// - **Deferred** draws the thin G-buffer with `raster_shading::thin`, then lights it in `light_thin` of hybrid_pass.sgl.
///
/// libs/graphics/shaped-viewer/docs/plans/hybrid-renderer.md is the design.
class sv::hybrid_routine : public sg::render_routine<hybrid_routine>
{
public:
    /// Records one frame into `cmd`, or declines while anything it draws through is still building, or on a device that does not trace.
    ///
    /// Must be called with no rendering scope open.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd, hybrid_frame_desc const& d);

    /// A deferred frame's thin G-buffer at `extent`, transient, for `hybrid_frame_desc::thin`.
    [[nodiscard]] static raster_thin_targets create_thin_targets(sg::context& ctx, tg::vec2i extent);

protected:
    /// Creates the stand-ins, which outlive every shader reload.
    cc::shared_async<cc::unit> init_once(sg::routine_init_scope scope) override;

    /// Builds the deferred lighting kernel and the guide blend.
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    impl::traced_stand_ins _stand_ins;

    /// What a forward frame binds as `traced.output`, which its pixel stages never write.
    sg::texture_2d _output;

    /// `light_thin` of hybrid_pass.sgl; null where it did not build, which declines a deferred frame.
    sg::compute_pipeline_handle _light_thin;

    /// `blend_guides` of hybrid_pass.sgl; null where it did not build, which declines a frame that blends its guides.
    sg::compute_pipeline_handle _blend_guides;
};
