#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/thread/async.hh>
#include <sgl_modules/raster.hh>
#include <sgl_modules/tracer.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/raster/raster_pipeline.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh> // sg::tlas_instance
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/rendering/raster_routine.hh> // sv::raster_draw
#include <shaped-viewer/rendering/stand_in_report.hh>
#include <shaped-viewer/scene/background.hh>
#include <shaped-viewer/view/render_settings.hh> // sv::screen_ambient_occlusion

/// Everything one bound raster frame binds and draws.
///
/// The draws are `raster_draw`s as the bindless mode takes them, and the routine skips the ones it cannot draw yet:
/// a quadric batch's, and a frame mesh's over its own index buffer, whose triangles the pixel stage would look up in the pool.
struct sv::raster_bound_frame_desc
{
    /// How many lanes of 16 bytes group 0's constant buffer holds for decals, `raster_bound.bound.decals`.
    static constexpr isize decal_lane_count = 1024;

    /// The camera, this layer's previous camera, the counts and the image's size.
    shaders::raster::frame_constants frame = {};

    /// The SH environment probe, which lights every surface and fills every pixel no surface covers.
    /// A value rather than a buffer: it is bound in the group's constant buffer.
    background_gpu background = {};

    /// One record per row; `raster_draw::instance` indexes it.
    sg::buffer<shaders::tracer::instance_record> instance_table;

    /// The decals, `frame.decal_count` `tracer::decal_record`s of 6 lanes each, then the shapes they cover of 5 lanes
    /// each, at most `decal_lane_count` lanes; empty for none.
    cc::span<tg::vec4f const> decal_lanes;

    /// The atlas the decals' shapes are drawn from; null for none.
    sr::slug_atlas const* decal_atlas = nullptr;

    /// Every light of the layer, `frame.light_count` of them; null for none, which the routine binds a stand-in for.
    sg::buffer<shaders::tracer::light_record> lights;

    cc::span<raster_draw const> draws;

    /// The textures each draw's instance binds as its material's `sv_textures`, one list per draw in `draws`' order.
    /// A list holds what `gpu_resource_manager::instance_textures` hands out; empty for an untextured material.
    cc::span<cc::vector<sg::texture_2d> const> draw_textures;

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

    /// Where each pixel's motion goes, rg16_float, in pixels: now minus last frame, both without `frame.jitter`.
    /// Null where nothing reads it.
    sg::texture_2d motion;

    /// Where set, receives what a frame that executed drew through `fallback` in place of `permutations`.
    stand_in_report* stand_ins = nullptr;

    /// Whether the opaque draws lay down their depth first, so each pixel is shaded once, by the surface visible there.
    /// Without it, an occluded surface drawn first is shaded and then overwritten; off is for measuring what the prepass saves.
    bool depth_prepass = true;

    /// The screen-space ambient occlusion the frame computes from the prepass's depth; needs `depth_prepass`.
    screen_ambient_occlusion ambient_occlusion = {};

    /// Where the occlusion accumulates over frames: last frame's half-size history, read, and this frame's, written; rgba32_float, the value, its frame count and its view depth.
    /// Null for none, which blurs each frame's occlusion alone.
    sg::texture_2d ao_history_read;
    sg::texture_2d ao_history_write;

    /// Whether `ao_history_read` holds last frame's accumulation, reprojectable through `frame.previous_camera`.
    bool ao_history_valid = false;

    /// How the frame finds its shadows; needs `depth_prepass`, and `traced` a device with `ray_query`.
    shadow_settings shadows = {};

    /// A hash of everything the shadow maps depend on, the instances and the lights; the maps are redrawn when it changes.
    u64 caster_key = 0;

    /// The lights as `lights` holds them, which the maps are drawn for: one tile each of the first 16 rects.
    cc::span<shaders::tracer::light_record const> light_records;

    /// The TLAS instances the shadow and indirect rays meet, each with its BLAS; empty traces neither.
    cc::span<sg::tlas_instance const> instances;

    /// Where the shadow share accumulates, as `ao_history_read` / `ao_history_write` do for the occlusion.
    sg::texture_2d shadow_history_read;
    sg::texture_2d shadow_history_write;
    bool shadow_history_valid = false;

    /// The indirect light the frame traces; needs `depth_prepass`, a device with `ray_query`, and `instances`.
    traced_indirect indirect = {};

    /// Where it accumulates, at its traced size, the full size divided by `indirect.scale` and rounded up: last frame's
    /// history, read, and this frame's, written; the radiance and its frame count rgba16_float, the depth r32_float.
    /// Null for none, which blurs each frame's rays alone.
    sg::texture_2d indirect_history_read;
    sg::texture_2d indirect_history_depth_read;
    sg::texture_2d indirect_history_write;
    sg::texture_2d indirect_history_depth_write;
    bool indirect_history_valid = false;

    /// The mirror reflections the frame finds on screen; needs `depth_prepass`.
    screen_space_reflections reflections = {};

    /// Where the traced reflections accumulate, at half the size: last frame's history, read, and this frame's, written;
    /// the radiance and its frame count rgba16_float, the depth r32_float.
    /// Null for none, which shows each frame's rays alone.
    sg::texture_2d reflection_history_read;
    sg::texture_2d reflection_history_depth_read;
    sg::texture_2d reflection_history_write;
    sg::texture_2d reflection_history_depth_write;
    bool reflection_history_valid = false;

    /// Last frame's image of this layer, which the reflections read; rgba32_float, never `color`.
    /// Null, or not yet drawn into, on a layer's first frame, and the frame finds no reflections.
    sg::texture_2d previous_image;
    bool previous_image_valid = false;

    /// Whether the reflections show their history length instead, white for 64 frames: a debugging view.
    bool debug_reflection_history = false;

    /// The frame's number, which turns the occlusion's sampling pattern from frame to frame.
    u32 frame_index = 0;
};

/// The bound raster mode's frame: one direct indexed draw per item, through each permutation's `bound` pixel stage, then the environment.
///
/// **Nothing here needs `binding_arrays`.**
/// Group 0 is `raster_bound.bound`, which binds the four central pools themselves where the bindless mode reaches them through table slots.
/// So it runs on WebGPU's core, whose limits group 0 is sized to: seven storage buffers, the eighth being sg's shader diagnostics, and two color targets in 20 bytes a pixel.
///
/// A textured material's draws bind group 1 too, its `sv_textures`: the draw's instance's textures and the permutation's samplers.
/// A draw is one instance whose first instance is its row, which `@instance_index` carries, since WGSL has no `first_instance`.
/// Without `primitive_index` the draws are pulled instead: no index buffer, three corners a triangle, each reading its index out of the pool.
/// Nothing is culled.
/// The opaque draws lay down their depth first, in a pass of their own, through the vertex stage the shading draws run; a cutout writes its depth as it shades.
/// From that depth, two half-size passes compute ambient occlusion -- a visibility bitmask, then a blur -- which the shading reads in group 0.
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

        /// 1x1, white: the occlusion a frame binds where it computed none, and in the passes computing it.
        sg::texture_2d no_occlusion;

        /// 1x1, zero: the reflection and its depth a frame binds where it found none, trusted nowhere.
        sg::texture_2d no_reflection;
        sg::texture_2d no_reflection_depth;

        /// 1x1, zero: the instance ids a pass binds where none are known yet, which every upsample then accepts.
        sg::texture_2d no_ids;

        /// 1x1: the decal atlas's tables a frame binds while it has no atlas, whose decals then read none.
        sg::texture_2d decal_curves;
        sg::texture_2d decal_bands;
    };
    stand_ins _stand_ins;

    /// `raster_bound_pass.sgl`'s vertex stage, which every material's pipeline shares: `bound_pulled_vs` where `_pulled`.
    sg::compiled_shader _surface_vs;

    /// Whether the device lacks `primitive_index`, so every draw is pulled: no index buffer, its corners read from the pool.
    bool _pulled = false;

    /// The environment pass of `raster_bound_pass.sgl`.
    sg::raster_pipeline_handle _environment;

    /// `_surface_vs` alone, writing depth and no color: the prepass; null where it did not build.
    sg::raster_pipeline_handle _depth_only;

    /// `bound_quadric_vs`, the vertex stage of every quadric batch's boxes; none where it did not compile, and no
    /// quadric batch draws.
    cc::optional<sg::compiled_shader> _quadric_vs;

    /// The prepass of a quadric's pixel, which solves its surface and writes its depth; null where it did not build.
    sg::raster_pipeline_handle _quadric_depth_only;

    /// The ambient occlusion, its accumulation and its blur, `raster_bound_pass.sgl`'s; null where they did not build,
    /// and the frame computes none.
    sg::raster_pipeline_handle _occlusion;
    sg::raster_pipeline_handle _occlusion_accumulate;
    sg::raster_pipeline_handle _occlusion_blur;

    /// `raster_bound_pass.sgl`'s reflection pass; null where it did not build, and the frame finds none.
    sg::raster_pipeline_handle _reflection;

    /// `raster_bound_indirect_pass.sgl`'s rays and their blur; null on a device without `ray_query`, or where they did
    /// not build.
    sg::raster_pipeline_handle _indirect;
    sg::raster_pipeline_handle _indirect_accumulate;
    sg::raster_pipeline_handle _indirect_blur;

    /// `raster_bound_indirect_pass.sgl`'s mirror rays where the screen's reflection failed; null where `_indirect` is.
    sg::raster_pipeline_handle _reflection_trace;

    /// `raster_bound_shadow_pass.sgl`'s shadow rays; null on a device without `ray_query`, or where it did not build.
    sg::raster_pipeline_handle _shadow;

    /// `raster_bound_shadow_map_pass.sgl`'s caster draws, depth alone into a light's tile (`bound_caster_pulled_vs` where
    /// `_pulled`), and the pass reading the maps; null where they did not build, and `maps` draws unshadowed.
    sg::raster_pipeline_handle _caster;
    sg::raster_pipeline_handle _shadow_map;

    /// Every rect light's depth map, a tile each, and the `caster_key` they were drawn for.
    /// Persistent: a still scene draws them once.
    sg::texture_2d _shadow_atlas;
    u64 _shadow_atlas_key = 0;

    /// The TLAS the shadow and indirect rays last met, and a hash of the instances it was built from.
    /// Kept while the instances hash the same: a build is a run of compute passes, and on webgpu each makes bind groups
    /// a browser frees only when it collects garbage, so rebuilding a still scene every frame stalls it.
    sg::tlas_handle _shadow_tlas;
    u64 _shadow_tlas_key = 0;

    /// A permutation's bound pixel stage and the pipeline over it, compiled and built on first use, polled rather than waited on.
    struct surface_pipeline
    {
        cc::shared_async<sg::compiled_shader> shader;
        sg::async_raster_pipeline pending;
        sg::raster_pipeline_handle pipeline;
        bool failed = false;

        /// The layout of the material's `sv_textures`, its group 2, read off the compiled stage; null for an untextured material.
        sg::binding_group_layout_handle textures_layout;

        /// Which of the material's texture slots that layout holds: the compiler drops a slot the stage never reads.
        cc::vector<bool> slot_bound;
    };

    /// The pipeline of permutation `p`, or null while it compiles or once it failed, which `surface_pipeline::failed` tells apart.
    [[nodiscard]] sg::raster_pipeline_handle _surface_pipeline_of(sg::context& ctx, material_permutation const* p);

    /// Keyed on the permutation's key; dropped by every reload.
    cc::map<cc::hash128, surface_pipeline> _surfaces;

    /// A textured draw's group 2, kept across frames rather than made anew each one: a scene of many textured draws
    /// would otherwise fill dx12's transient descriptor region within a frame.
    struct texture_group
    {
        sg::binding_group_handle group;
        u64 last_used = 0;
    };

    /// Keyed on the layout, the permutation and the textures bound; one unused for `texture_group_idle_frames` is dropped.
    cc::map<u64, texture_group> _texture_groups;
};
