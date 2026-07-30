#pragma once

#include <clean-core/container/span.hh>
#include <shaped-graphics/acceleration_structure.hh> // sg::tlas_instance
#include <shaped-graphics/buffer.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/render_routine.hh>
#include <shaped-graphics/texture.hh>
#include <shaped-viewer/background.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/pbr_material.hh>
#include <shaped-viewer/rendering/frame_constants.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sv
{
/// Everything one view's trace binds.
/// The renderer fills this from the resolved scene; the routine owns the per-frame TLAS build and the dispatch.
struct trace_desc
{
    sg::buffer<frame_constants_gpu> frame;       // the FrameConstants cbuffer (camera + directional light)
    sg::buffer<background_gpu> background;       // the Background cbuffer (SH environment probe) the miss reads
    cc::span<sg::tlas_instance const> instances; // one per scene item; the TLAS is (re)built from these
    sg::texture_2d output;                       // rgba16f UAV the raygen writes
    sg::buffer<pbr_material_gpu> materials;      // per-triangle PBR params, indexed by PrimitiveIndex()
    sg::buffer<tg::pos3f> vertices;              // the hit mesh's positions, for the flat face normal
    sg::buffer<u32> indices;                     // 3 indices per triangle, into `vertices`
    tg::vec2i size;                              // dispatch extent (= output size)
};

/// The basic flat-shaded PBR ray-tracing pass.
///
/// A render routine (see the "everything that traces is a routine" rule): it owns the DXR pipeline + shader table + global root signature, built once in `init_declare` from the slib-acquired shaders and rebuilt on reload.
/// `execute` (re)builds the frame's TLAS, binds the scene, and dispatches one ray per pixel into the output image.
/// `execute` only reads what `init_declare` built, so it takes the const `acquire` and holds no lock — concurrent traces on the same context do not serialize on this routine.
class pbr_raytrace_routine : public sg::render_routine<pbr_raytrace_routine>
{
public:
    /// Builds the TLAS from `d.instances`, binds the scene, and dispatches `d.size` primary rays into `d.output`.
    /// A no-op (leaves the target untouched) if the shaders did not compile.
    static void execute(sg::command_list& cmd, trace_desc const& d);

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
} // namespace sv
