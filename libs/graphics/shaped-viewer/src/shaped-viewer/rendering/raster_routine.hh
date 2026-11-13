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
#include <shaped-viewer/rendering/stand_in_report.hh>
#include <shaped-viewer/scene/background.hh>

/// What a raster frame writes: the lit image, and the primary hit's G-buffer beside it.
///
/// **This is the seam the hybrid integrator grows from.**
/// Every target is the layer's slot of the same meaning, in the format the tracer writes its guides in, so a lighting pass after the raster reads either path's alike.
/// A pixel no surface covers holds the environment in `color`, its reprojection in `motion`, 1 in `primary_depth`, and zero in every other target.
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

    /// How far each pixel's bounce went, the diffuse half in r and the specular in g, rg32_float, or null where nothing reads it.
    /// Only a deferred hybrid frame writes it, from its lighting kernel; a raster pass never does.
    sg::texture_2d hit_distance;
};

/// The hybrid's thin G-buffer, which a deferred frame's raster pass writes beside the depth, motion and primary depth of `raster_gbuffer`.
///
/// The material reduced to a closure a compute kernel rebuilds; `module raster`'s `thin_targets` is the whole set in SGL, and says how each is packed.
/// Every texture is at the frame's extent and created with `render_target` and `texture` usage.
struct sv::raster_thin_targets
{
    /// The surface's own emission, rgba16_float.
    sg::texture_2d emission;

    /// The shading and the geometric normal, both octahedral, rgba16_float.
    sg::texture_2d normals;

    /// The base color, square-rooted, and the metalness, rgba8_unorm.
    sg::texture_2d base;

    /// The specular color, square-rooted, and the specular roughness, rgba8_unorm.
    sg::texture_2d specular;

    /// The coat's weight and roughness, the specular IOR and the transmission's weight, rgba8_unorm.
    sg::texture_2d extra;
};

/// What a raster frame renders through and nobody reads after it: its depth buffer, and the depth pyramid occlusion culling builds.
/// Both at the frame's extent; `raster_routine::create_scratch` makes them.
struct sv::raster_scratch
{
    sg::texture_2d depth;
    sg::texture_2d pyramid;
};

/// What a raster frame's shading draws are: which pixel stage of each permutation they run, through which vertex stage, into which targets.
enum class sv::raster_shading : sv::u8
{
    /// `sv_raster`, lit without tracing, into `raster_frame_desc::targets`: the raster path.
    lit,

    /// `sv_gbuffer`, the hybrid's thin G-buffer, into `raster_frame_desc::thin` and the depth, motion and primary depth of `targets`.
    /// What a deferred hybrid frame draws before its lighting kernel.
    thin,

    /// `sv_hybrid`, lit with its shadow rays traced inline, into `targets`: a forward hybrid frame.
    /// Group 0 is then `raster_frame_desc::traced` rather than `raster.drawn`, and the vertex stage is hybrid_pass.sgl's.
    traced,
};

/// One draw of a raster frame: every triangle of one row of the instance table, through one permutation's pixel stage.
///
/// An indexed draw over `raster_frame_desc::index_pool`: the row's geometry range, as its instance record names it;
/// or over a frame mesh's own index buffer, `indices`.
struct sv::raster_draw
{
    /// The row of `raster_frame_desc::instance_table` the draw is.
    u32 instance = 0;

    /// How many triangles the row's geometry holds; 0 for a quadric batch's row.
    u32 triangle_count = 0;

    /// How many primitives the row's quadric batch holds, each drawn as its box; 0 for a mesh's row.
    u32 primitive_count = 0;

    /// Which of `raster_frame_desc::permutations` shades it.
    u32 permutation = 0;

    /// Where the row's indices start in the index pool, and the vertex they are relative to.
    u32 first_index = 0;
    u32 base_vertex = 0;

    /// The row's geometry's box in its own space, which the GPU culls the draw by through the row's placement; none draws it always.
    cc::optional<tg::aabb3f> bounds;

    /// A frame mesh's own index buffer, which the draw binds in place of the pool, from its first index; null draws through the pool.
    /// Such a draw is never culled and is drawn directly, once, in the frame's first pass.
    sg::buffer<u32> indices;
};

/// How far a raster frame culls: `occlusion` where it has a visibility record, else `view`; `none` draws every item.
/// Fewer than `occlusion` is for measuring what culling saves.
enum class sv::raster_culling : sv::u8
{
    none,
    view,
    occlusion,
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

    /// The mesh manager's index pool, which every draw is indexed through; `mesh_manager::index_pool`.
    sg::buffer<u32> index_pool;

    /// The permutations the draws shade with; each must be a triangle one.
    /// One whose pixel stage has not compiled is drawn through `fallback`, as a trace substitutes a hit group.
    cc::span<material_permutation const* const> permutations;

    /// The neutral triangle permutation a permutation still compiling is drawn with; null declines the frame instead.
    material_permutation const* fallback = nullptr;

    /// The manager's bindless tables, bound as the pipeline's second group for the length of the frame.
    bound_resources const* bindless = nullptr;

    /// Where the frame is written, every target at one extent.
    raster_gbuffer targets;

    /// Where set, receives what a frame that executed drew through `fallback` in place of `permutations`.
    stand_in_report* stand_ins = nullptr;

    /// Where set, receives the arguments the frame drew from, after culling, grouped by permutation in `draws` order within each.
    /// With occlusion culling, those of the second pass.
    /// For a test reading back what was culled.
    sg::buffer<sg::draw_indexed_args>* drawn_args = nullptr;

    /// Where set, the frame culls by occlusion too, and this is what each draw was last frame: 1 visible, 0 not.
    /// The caller keeps it from frame to frame, one per layer, and the routine rewrites it each frame.
    /// It is recreated, everything not visible, whenever the frame holds another count of draws; a stale one costs draws and never hides one.
    sg::buffer<u32>* visibility = nullptr;

    /// At most how far the frame culls; occlusion also needs `visibility`.
    raster_culling culling = raster_culling::occlusion;

    /// Whether the opaque draws lay down their depth first, so each pixel is shaded once, by the surface that is visible there.
    /// Without it, an occluded surface drawn first is shaded and then overwritten; off is for measuring what the prepass saves.
    bool depth_prepass = true;

    /// Which pixel stage the shading draws run, and what they write.
    raster_shading shading = raster_shading::lit;

    /// With `thin` shading, where the thin G-buffer is written; unread otherwise.
    raster_thin_targets thin;

    /// The frame's depth buffer and depth pyramid, from `raster_routine::create_scratch` at the frame's extent; empty creates them.
    /// A caller drawing several frames of one extent in one command list passes one, so they share it rather than each allocating its own.
    raster_scratch scratch;

    /// With `traced` shading, `tracer.traced` for this frame, which every stage reads as group 0; unread otherwise.
    /// Its images must not be any of `targets`, which the frame renders to while the group is bound.
    sg::binding_group_handle traced;
};

/// The raster path: one indexed draw per scene item, through each material permutation's generated pixel stage, into a G-buffer.
///
/// **The GPU decides what draws.** A cull dispatch tests each item's box against the camera's view and writes its draw's arguments, an instance count of 0 for one out of view.
/// The CPU then records one indirect draw per permutation, whatever was culled.
///
/// **Depth comes first.** A depth-only pipeline draws every opaque draw before anything is shaded, and the shading draws then test against that depth without writing it.
/// So a pixel stage runs once per visible pixel, which is what makes an expensive one, such as the hybrid's traced lighting, affordable.
/// A cutout draw's depth depends on its material, so it skips the prepass and writes its depth as it shades, after the opaque ones.
///
/// A draw's first instance is its row of the instance table, which the vertex stage takes as `@first_instance`.
/// Its pixel stage is one the permutation's sources hold, by `raster_frame_desc::shading`: the material body the tracer's hit group evaluates, then what lights it.
/// The raster path's `raster.shade` lights the surface directly from every light, unshadowed, and from the environment's SH, and writes the G-buffer.
/// The environment fills whatever no surface covered, last.
///
/// The bindings come in three groups, as the tracer's do: `raster.drawn`, the manager's bindless tables, and `slug.tables` for the decals.
/// A forward hybrid frame binds `tracer.traced` as the first instead, and the image's size as inline constants.
/// libs/graphics/shaped-viewer/docs/raster.md is the design, and libs/graphics/shaped-viewer/docs/plans/hybrid-renderer.md the hybrid's.
class sv::raster_routine : public sg::render_routine<raster_routine>
{
public:
    /// Records one frame into `cmd`, or declines while the vertex stage, the environment, the prepass or every stand-in is still building.
    ///
    /// Must be called with no rendering scope open: it opens its own over `d.targets`.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd, raster_frame_desc const& d);

    /// A frame's depth buffer and depth pyramid at `extent`, transient, for `raster_frame_desc::scratch`.
    [[nodiscard]] static raster_scratch create_scratch(sg::context& ctx, tg::vec2i extent);

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
    /// The vertex stage of a quadric batch's boxes, beside `_surface_vs`.
    sg::compiled_shader _quadric_vs;

    /// `hybrid_pass.sgl`'s vertex stage, over `tracer.traced`; none where the device does not trace, which declines a `traced` frame.
    cc::optional<sg::compiled_shader> _hybrid_vs;
    cc::optional<sg::compiled_shader> _hybrid_quadric_vs;

    /// The environment pass of `raster_pass.sgl`, into the lit targets and into the thin ones.
    sg::raster_pipeline_handle _environment;
    sg::raster_pipeline_handle _environment_thin;

    /// `raster_cull.sgl`'s kernel; null where it failed to build, and a frame then draws every item.
    sg::compute_pipeline_handle _cull;

    /// The two-phase occlusion kernels of `raster_cull.sgl`, all four or none; without them a frame culls by the view alone.
    struct occlusion_kernels
    {
        sg::compute_pipeline_handle cull_first;
        sg::compute_pipeline_handle pyramid_seed;
        sg::compute_pipeline_handle pyramid_seed_depth;
        sg::compute_pipeline_handle pyramid_down;
        sg::compute_pipeline_handle cull_second;
    };
    cc::optional<occlusion_kernels> _occlusion;

    /// One pixel stage of a permutation and the pipelines over it, compiled and built on first use, polled rather than waited on.
    /// Two pipelines share the stage: index 0 tests against a prepass's depth without writing it, and 1 writes its own.
    struct surface_stage
    {
        cc::shared_async<sg::compiled_shader> shader;
        sg::async_raster_pipeline pending[2];
        sg::raster_pipeline_handle pipeline[2];
        bool failed = false;
    };

    /// A permutation's stages, one per `raster_shading`, each started only once a frame shades with it.
    struct surface_pipeline
    {
        surface_stage stages[3];
    };

    /// The pipeline of `p` for `shading` that writes depth or only tests it, or null while it builds and after it failed; never waits.
    [[nodiscard]] sg::raster_pipeline_handle _surface_pipeline_of(sg::context& ctx,
                                                                  material_permutation const* p,
                                                                  bound_resources const& bindless,
                                                                  raster_shading shading,
                                                                  bool writes_depth);

    /// Keyed on the permutation's key and the bindless layout the pipeline was built against; dropped by every reload.
    cc::map<cc::hash128, surface_pipeline> _surfaces;

    /// The depth-only pipeline, built on first use like a surface's.
    struct depth_pipeline
    {
        sg::async_raster_pipeline pending;
        sg::raster_pipeline_handle pipeline;
        bool failed = false;
    };

    /// Keyed on the bindless layout the pipeline was built against and on its vertex stage; dropped by every reload.
    cc::map<cc::hash128, depth_pipeline> _depth_only;

    /// The depth-only pipeline over the vertex stage `traced` shading draws with or the shared one, its build started on first use; never waits.
    [[nodiscard]] depth_pipeline const& _depth_pipeline_of(sg::context& ctx, bound_resources const& bindless, bool traced);
};
