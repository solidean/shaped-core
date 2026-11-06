#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/thread/async.hh>
#include <sgl_modules/raster.hh>
#include <sgl_modules/tracer.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/rendering/raster_routine.hh> // sv::raster_draw
#include <shaped-viewer/rendering/stand_in_report.hh>
#include <shaped-viewer/scene/background.hh>

/// Everything one bound raster frame binds and draws.
///
/// The draws are `raster_draw`s as the bindless mode takes them, and the routine skips the ones it cannot draw yet:
/// a quadric batch's, and a frame mesh's over its own index buffer, whose triangles the pixel stage would look up in the pool.
struct sv::raster_bound_frame_desc
{
    /// The camera, this layer's previous camera, the counts and the image's size.
    shaders::raster::frame_constants frame = {};

    /// The SH environment probe, which lights every surface and fills every pixel no surface covers.
    sg::buffer<background_gpu> background;

    /// One record per row; `raster_draw::instance` indexes it.
    sg::buffer<shaders::tracer::instance_record> instance_table;

    /// Every light of the layer, `frame.light_count` of them; null for none, which the routine binds a stand-in for.
    sg::buffer<shaders::tracer::light_record> lights;

    cc::span<raster_draw const> draws;

    /// The four central pools a pixel reads its surface from, as the resource manager holds them this frame.
    /// The parameter and the attribute pool may still be null, before anything was allocated in them.
    sg::buffer<tg::pos3f> vertex_pool;
    sg::buffer<u32> index_pool;
    sg::buffer<byte> parameter_pool;
    sg::buffer<byte> attribute_pool;

    /// The permutations the draws shade with; each must be a triangle one.
    cc::span<material_permutation const* const> permutations;

    /// The neutral triangle permutation a permutation still compiling is drawn with; null declines the frame instead.
    material_permutation const* fallback = nullptr;

    /// The lit radiance, rgba32_float: the layer's accumulation slot.
    sg::texture_2d color;

    /// The clip depth a layer's drawings are occluded by, r32_float, or null where nothing reads it.
    sg::texture_2d primary_depth;

    /// Where set, receives what a frame that executed drew through `fallback` in place of `permutations`.
    stand_in_report* stand_ins = nullptr;
};

/// The bound raster mode's frame: one direct indexed draw per item, through each permutation's `bound` pixel stage, then the environment.
///
/// **Nothing here needs `binding_arrays`.**
/// Group 0 is `raster_bound.bound`, which binds the four central pools themselves where the bindless mode reaches them through table slots.
/// So it runs on WebGPU's core, whose limits group 0 is sized to: eight storage buffers, two color targets in 20 bytes a pixel.
///
/// A draw is one instance whose first instance is its row, which `@instance_index` carries, since WGSL has no `first_instance`.
/// Nothing is culled and there is no depth prepass yet: the surfaces write depth as they shade, which the environment then tests against.
/// libs/graphics/shaped-viewer/docs/plans/raster-bound.md is the design.
class sv::raster_bound_routine : public sg::render_routine<raster_bound_routine>
{
public:
    /// Records one frame into `cmd`, or declines while the vertex stage, the environment or every stand-in is still building.
    ///
    /// Must be called with no rendering scope open: it opens its own over `d.color` and `d.primary_depth`.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd, raster_bound_frame_desc const& d);

protected:
    /// Creates the stand-ins, which outlive every shader reload.
    cc::shared_async<cc::unit> init_once(sg::routine_init_scope scope) override;

    /// Compiles the vertex stage and builds the environment pipeline; drops every material's pipeline, which a reload makes stale.
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    /// What a frame binds where it has nothing of its own.
    struct stand_ins
    {
        sg::buffer<shaders::tracer::light_record> lights;
        sg::buffer<byte> bytes;
    };
    stand_ins _stand_ins;

    /// `raster_bound_pass.sgl`'s vertex stage, which every material's pipeline shares.
    sg::compiled_shader _surface_vs;

    /// The environment pass of `raster_bound_pass.sgl`.
    sg::raster_pipeline_handle _environment;

    /// A permutation's bound pixel stage and the pipeline over it, compiled and built on first use, polled rather than waited on.
    struct surface_pipeline
    {
        cc::shared_async<sg::compiled_shader> shader;
        sg::async_raster_pipeline pending;
        sg::raster_pipeline_handle pipeline;
        bool failed = false;
    };

    /// The pipeline of permutation `p`, or null while it compiles or once it failed, which `surface_pipeline::failed` tells apart.
    [[nodiscard]] sg::raster_pipeline_handle _surface_pipeline_of(sg::context& ctx, material_permutation const* p);

    /// Keyed on the permutation's key; dropped by every reload.
    cc::map<cc::hash128, surface_pipeline> _surfaces;
};
