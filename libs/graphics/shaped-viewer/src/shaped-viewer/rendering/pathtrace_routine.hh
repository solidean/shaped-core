#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh> // sg::tlas_instance
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/gpu_types.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/scene/background.hh>
#include <shaped-viewer/scene/light.hh> // area_light_gpu
#include <shaped-viewer/scene/pbr_material.hh>
#include <shaped-viewer/view/camera.hh> // camera_gpu
#include <typed-geometry/linalg/pos.hh>

/// The per-view constant block the path tracer reads at b0 (the FrameConstants cbuffer in shaders/pt_common.hlsli).
/// Mirrors that cbuffer lane-for-lane — keep them in lockstep.
///
/// Beyond the camera it carries the single rectangular area light the integrator samples for direct lighting, plus the two path-tracer controls (samples-per-pixel and bounce depth).
/// Laid out as 16-byte lanes to match HLSL cbuffer packing: each `vec3` pairs with the scalar after it to fill one lane.
///
/// The view_renderer fills the light with `area_light_gpu::from(view's area_light)`.
struct sv::pt_frame_constants_gpu
{
    camera_gpu camera;
    area_light_gpu light;

    i32 samples_per_pixel = 16; // primary rays integrated per pixel, accumulated in the one dispatch
    i32 max_bounces = 5;        // path length: primary hit + this many diffuse bounces
    u32 seed = 1;               // per-frame RNG seed; vary it to decorrelate accumulated frames
    u32 accum_frame = 0;        // progressive accumulation index: 0 restarts, >0 blends into the output in place

    /// false => the bound mesh is a non-indexed triangle list, so the closest-hit reads its vertices directly
    /// instead of through `Indices`. Set it from `mesh_record::is_indexed`; the trace binds one mesh per view,
    /// which is why per-mesh state can ride here at all.
    sr::gpu_boolean mesh_is_indexed = false;
    f32 _padding5[3] = {};

    /// The camera the previous recorded frame traced from, which is what this frame reprojects its history through.
    /// Meaningless unless `has_history`.
    camera_gpu prev_camera;

    /// Whether the bound history textures hold a previous frame at all.
    /// False on the first frame after any reset, where every pixel starts its estimate from nothing.
    sr::gpu_boolean has_history = false;

    /// Ceiling on the per-pixel sample count a reprojected pixel may carry forward.
    ///
    /// The hybrid the viewer wants: left at `u32(-1)` while the camera is still, so a static view keeps the exact
    /// running mean and converges to ground truth; lowered once it moves, so a sample dragged along by reprojection
    /// ages out instead of smearing indefinitely.
    u32 history_max_frames = u32(-1);

    /// 0 renders normally; 1 replaces the written color with a false-color of each pixel's carried sample count.
    ///
    /// The one direct way to tell a *rejected* pixel from a merely high-variance one: rejection pins a pixel at
    /// zero carried samples forever and reads as permanent noise, while a converging pixel just converges slowly.
    /// Red is "kept nothing this frame", ramping through to green as the count climbs.
    ///
    /// It deliberately poisons the accumulated color while enabled, since that color is what the history stores.
    /// The count itself keeps evolving untouched, which is what is being inspected; turning it off recovers.
    u32 debug_view = 0;

    // Pad the block to a full 256-byte CBV range (see frame_constants.hh).
    f32 _reserved[1] = {};
};

namespace sv
{

static_assert(sizeof(pt_frame_constants_gpu) == 256, "pt_frame_constants_gpu must be a full 256-byte CBV block");

} // namespace sv

/// Everything one view's path trace binds.
/// Mirrors trace_desc, but the frame block is a pt_frame_constants_gpu — it carries the area light and the sample controls the integrator needs.
struct sv::pt_trace_desc
{
    sg::buffer<pt_frame_constants_gpu> frame;    // the FrameConstants cbuffer (camera + light + sample controls)
    sg::buffer<background_gpu> background;       // the Background cbuffer (SH environment probe) the miss reads
    cc::span<sg::tlas_instance const> instances; // one per scene item; the TLAS is (re)built from these
    sg::texture_2d output;                       // rgba16f UAV the raygen writes the integrated color into

    /// rgba16f UAV taking this frame's primary-hit geometry: `float4(normal.xyz, hit_t)`, `hit_t < 0` where the ray escaped.
    /// Same extent as `output` — the raygen writes both at the dispatch's own pixel.
    sg::texture_2d gbuffer;

    /// The previous recorded frame's `output` and `gbuffer`, sampled to reproject the history onto this frame.
    ///
    /// Must not alias `output` / `gbuffer`: the raygen reads them at a *different* pixel than it writes.
    /// Both must be bound even when `pt_frame_constants_gpu::has_history` is false — the shader ignores their
    /// contents, but the binding group still needs a view.
    sg::texture_2d history_color;
    sg::texture_2d history_gbuffer;

    sg::buffer<pbr_material_gpu> materials; // per-triangle PBR params, indexed by PrimitiveIndex()
    sg::buffer<tg::pos3f> vertices;         // the hit mesh's positions, for the flat face normal
    sg::buffer<u32> indices;                // 3 indices per triangle, into `vertices`
};

/// The global-illumination path-tracing pass.
///
/// A render routine, structured exactly like pbr_raytrace_routine.
/// It owns the DXR pipeline + shader table + global root signature, built once in `init_declare` from the slib-acquired path-tracing shaders and rebuilt on reload.
/// Where the flat PBR routine shades a single direct-lit sample, this one integrates global illumination.
/// The raygen bounces each ray diffusely and estimates direct light at every hit by next-event estimation toward two sources: the rectangular area light and the SH environment.
/// The environment is gathered by multiple importance sampling (balance heuristic) between that NEE ray and the escaped bounce ray, keeping a bright, non-uniform sky low-variance.
/// `samples_per_pixel` paths per pixel accumulate in one dispatch; `shaders/pathtrace.hlsl` carries the estimators themselves.
/// `execute` only reads what `init_declare` built, so it takes the const `acquire` and holds no lock — concurrent traces on the same context do not serialize on this routine.
class sv::pathtrace_routine : public sg::render_routine<pathtrace_routine>
{
public:
    /// Builds the TLAS from `d.instances`, binds the scene, and integrates one path bundle per pixel over `d.output`'s extent into `d.output`.
    /// A no-op (leaves the target untouched) if the shaders did not compile.
    static void execute(sg::command_list& cmd, pt_trace_desc const& d);

    /// Whether this routine has a pipeline to dispatch — false when the shaders did not compile or the bindings did not merge.
    ///
    /// `execute` degrades to a no-op rather than throwing, which is the right behavior for a live reload and the
    /// wrong one for a test: a broken shader then leaves an untouched target that no CPU-side assertion notices.
    /// So a test asserts on this, and a debug overlay can say why the image is empty.
    [[nodiscard]] static bool is_ready(sg::command_list& cmd);

protected:
    void init_declare(sg::context& ctx) override;

private:
    // Rebuilt wholesale by init_declare on every reload.
    // The pipeline layout is not among them — the pipeline holds it.
    sg::binding_group_layout_handle _group_layout;
    sg::raytracing_pipeline_handle _pipeline;
    sg::raytracing_shader_table_handle _table;
    sg::raygen_index _raygen = {};
};
