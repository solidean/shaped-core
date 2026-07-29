#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/acceleration_structure.hh> // sg::tlas_instance
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/render_routine.hh>
#include <shaped-graphics/texture.hh>
#include <shaped-viewer/background.hh>
#include <shaped-viewer/camera.hh> // camera_gpu
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/gpu_types.hh>
#include <shaped-viewer/light.hh> // area_light_gpu
#include <shaped-viewer/pbr_material.hh>
#include <typed-geometry/linalg/pos.hh>

namespace sv
{
/// The per-view constant block the path tracer reads at b0 (the FrameConstants cbuffer in shaders/pt_common.hlsli).
/// Mirrors that cbuffer lane-for-lane — keep them in lockstep.
///
/// Beyond the camera it carries the single rectangular area light the integrator samples for direct lighting, plus the two path-tracer controls (samples-per-pixel and bounce depth).
/// Laid out as 16-byte lanes to match HLSL cbuffer packing: each `vec3` pairs with the scalar after it to fill one lane.
///
/// The view_renderer fills the light with `area_light_gpu::from(view's area_light)`.
struct pt_frame_constants_gpu
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
    gpu_boolean mesh_is_indexed = false;
    f32 _padding5[3] = {};

    // Pad the block to a full 256-byte CBV range (see frame_constants.hh).
    // The tail is the seam for the next per-frame path-tracer controls (temporal accumulation index, low-discrepancy sequence offset, ...).
    f32 _reserved[20] = {};
};

static_assert(sizeof(pt_frame_constants_gpu) == 256, "pt_frame_constants_gpu must be a full 256-byte CBV block");

/// Everything one view's path trace binds.
/// Mirrors trace_desc, but the frame block is a pt_frame_constants_gpu — it carries the area light and the sample controls the integrator needs.
struct pt_trace_desc
{
    sg::buffer<pt_frame_constants_gpu> frame;    // the FrameConstants cbuffer (camera + light + sample controls)
    sg::buffer<background_gpu> background;       // the Background cbuffer (SH environment probe) the miss reads
    cc::span<sg::tlas_instance const> instances; // one per scene item; the TLAS is (re)built from these
    sg::texture_2d output;                       // rgba16f UAV the raygen writes the integrated color into
    sg::buffer<pbr_material_gpu> materials;      // per-triangle PBR params, indexed by PrimitiveIndex()
    sg::buffer<tg::pos3f> vertices;              // the hit mesh's positions, for the flat face normal
    sg::buffer<u32> indices;                     // 3 indices per triangle, into `vertices`
};

/// The global-illumination path-tracing pass.
///
/// A render routine, structured exactly like pbr_raytrace_routine: it owns the DXR pipeline + shader table + global root signature, built once in `init_declare` from the slib-acquired path-tracing shaders and rebuilt on reload.
/// Where the flat PBR routine shades a single direct-lit sample, this one integrates global illumination — the raygen bounces each ray diffusely and estimates direct light at every hit with next-event estimation toward the rectangular ceiling light, accumulating `samples_per_pixel` paths per pixel in one dispatch.
/// All state sits behind one mutex taken for the whole call.
class pathtrace_routine : public sg::render_routine<pathtrace_routine>
{
public:
    /// Builds the TLAS from `d.instances`, binds the scene, and integrates one path bundle per pixel over `d.output`'s extent into `d.output`.
    /// A no-op (leaves the target untouched) if the shaders did not compile.
    static void execute(sg::command_list& cmd, pt_trace_desc const& d);

protected:
    void init_declare(sg::context& ctx) override;

private:
    /// Everything the routine mutates, rebuilt wholesale by init_declare on every reload.
    struct state
    {
        sg::binding_group_layout_handle group_layout;
        sg::pipeline_layout_handle pipeline_layout;
        sg::raytracing_pipeline_handle pipeline;
        sg::raytracing_shader_table_handle table;
        sg::raygen_index raygen = {};
    };

    cc::mutex<state> _state;
};
} // namespace sv
