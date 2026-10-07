#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/thread/async.hh>
#include <sgl_modules/raster.hh> // sv::shaders::raster::frame_constants
#include <sgl_modules/tracer.hh> // the records a raster frame shares with the tracer
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/background.hh>

/// What a raster frame writes: the lit image, and the primary hit's G-buffer beside it.
///
/// **This is the seam the hybrid integrator grows from.**
/// Every target is the layer's slot of the same meaning, in the format the tracer writes its guides in, so a lighting pass after the raster reads either path's alike.
/// A pixel no surface covers holds the environment in `color`, zero in every G-buffer target, and 1 in `primary_depth`.
/// Every texture is at one extent and created with `render_target` usage; `module raster`'s `targets` is the same set in SGL.
struct sv::raster_gbuffer
{
    /// The lit radiance, alpha 1, rgba32_float: the layer's accumulation slot.
    sg::texture_2d color;

    /// The geometric normal turned toward the viewer, world space, rgba16_float.
    sg::texture_2d normal;

    /// The linear view depth along the camera's forward axis, r32_float.
    sg::texture_2d depth;

    /// Diffuse reflectance, rgba16_float.
    sg::texture_2d albedo;

    /// Specular reflectance at normal incidence, rgba16_float.
    sg::texture_2d specular_albedo;

    /// The sharpest specular lobe's perceptual roughness, r16_float.
    sg::texture_2d roughness;

    /// This pixel minus where its surface sat on the previous frame's image, in pixels, rg32_float.
    sg::texture_2d motion;

    /// The clip depth `1 - near / depth` a layer's drawings are occluded by, r32_float, or null where nothing reads it.
    sg::texture_2d primary_depth;
};

/// One draw of a raster frame: every triangle of one row of the instance table, through one permutation's pixel stage.
struct sv::raster_draw
{
    /// The row of `raster_frame_desc::instance_table` the draw is.
    u32 instance = 0;

    /// How many triangles the row's geometry holds; the draw is three vertices per triangle and no vertex buffer.
    u32 triangle_count = 0;

    /// Which of `raster_frame_desc::permutations` shades it.
    u32 permutation = 0;
};

/// Everything one raster frame binds and draws.
struct sv::raster_frame_desc
{
    /// The camera, this layer's previous camera, the counts and the image's size.
    shaders::raster::frame_constants frame = {};

    /// The SH environment probe, which lights every surface and fills every pixel no surface covers.
    sg::buffer<background_gpu> background;

    /// One record per row, as the tracer's instance table holds them; `raster_draw::instance` indexes it.
    sg::buffer<shaders::tracer::instance_record> instance_table;

    /// Every light of the layer, `frame.light_count` of them; null for none, which the routine binds a stand-in for.
    sg::buffer<shaders::tracer::light_record> lights;

    /// The decals, `frame.decal_count` of them, and the shapes they cover in `decal_atlas`; null for none.
    sg::buffer<shaders::tracer::decal_record> decals;
    sg::buffer<shaders::tracer::decal_shape> decal_shapes;
    sr::slug_atlas const* decal_atlas = nullptr;

    cc::span<raster_draw const> draws;

    /// The permutations the draws shade with; each must be a triangle one.
    /// One whose pixel stage has not compiled is drawn through `fallback`, as a trace substitutes a hit group.
    cc::span<material_permutation const* const> permutations;

    /// The neutral triangle permutation a permutation still compiling is drawn with; null declines the frame instead.
    material_permutation const* fallback = nullptr;

    /// The manager's bindless tables, bound as the pipeline's second group for the length of the frame.
    bound_resources const* bindless = nullptr;

    /// Where the frame is written, every target at one extent.
    raster_gbuffer targets;

    /// Where set, receives whether a frame that executed drew any permutation through `fallback`.
    bool* drawn_with_stand_ins = nullptr;
};

/// The raster path: one draw per scene item, through each material permutation's generated pixel stage, into a G-buffer.
///
/// A draw pulls its triangles out of the bindless tables in the vertex stage, three vertices per triangle and no vertex buffer, so one vertex stage serves every mesh.
/// Its pixel stage is the permutation's `raster_source`: the material body the tracer's hit group evaluates, then `raster.shade`.
/// That lights the surface directly from every light, unshadowed, and from the environment's SH, and writes the G-buffer.
/// The environment fills whatever no surface covered, last.
///
/// The bindings come in three groups, as the tracer's do: `raster.drawn`, the manager's bindless tables, and `slug.tables` for the decals.
/// Which row a draw is rides as inline constants, `raster.draw`.
/// libs/graphics/shaped-viewer/docs/raster.md is the design.
class sv::raster_routine : public sg::render_routine<raster_routine>
{
public:
    /// Records one frame into `cmd`, or declines while the vertex stage, the environment or every stand-in is still building.
    ///
    /// Must be called with no rendering scope open: it opens its own over `d.targets`.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd, raster_frame_desc const& d);

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
        sg::buffer<shaders::tracer::decal_record> decals;
        sg::buffer<shaders::tracer::decal_shape> decal_shapes;
        sg::texture_2d decal_curves;
        sg::texture_2d decal_bands;
    };
    stand_ins _stand_ins;

    /// `raster_pass.sgl`'s vertex stage, which every material's pipeline shares.
    sg::compiled_shader _surface_vs;

    /// The environment pass of `raster_pass.sgl`.
    sg::raster_pipeline_handle _environment;

    /// One permutation's pixel stage and the pipeline over it, compiled and built on first use, polled rather than waited on.
    struct surface_pipeline
    {
        cc::shared_async<sg::compiled_shader> shader;
        sg::async_raster_pipeline pending;
        sg::raster_pipeline_handle pipeline;
        bool failed = false;
    };

    /// The pipeline of `p`, or null while it builds and after it failed; never waits.
    [[nodiscard]] sg::raster_pipeline_handle _surface_pipeline_of(sg::context& ctx,
                                                                  material_permutation const* p,
                                                                  bound_resources const& bindless);

    /// Keyed on the permutation's key; dropped by every reload.
    cc::map<cc::hash128, surface_pipeline> _surfaces;
};
