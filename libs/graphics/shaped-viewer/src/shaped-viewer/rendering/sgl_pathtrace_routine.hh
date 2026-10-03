#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-shader-library/raytracing_pipeline.hh> // slib::raytracing_host_parts
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/rendering/pathtrace_routine.hh> // pt_trace_desc

/// The path tracer's SGL port: shaders/tracer_pipeline.sgl, with module `tracer` (shaders/sgl).
///
/// Side by side with `pathtrace_routine` until an image-parity test has proven it, and nothing a viewer runs reaches it yet.
/// It takes the same `pt_trace_desc` and integrates the same estimate with the same random stream.
///
/// **Every instance shades with one fixed material**, sv's neutral fallback (`material_shader_cache::acquire_fallback`).
/// Its one hit group is compiled at run time from SGL, the stand-in for what the material generator will write per permutation.
/// So `d.hit_groups`, `fallback` and `quadric_fallback` are not read, and each instance's `hit_group_offset` is replaced by the group's row.
///
/// What the SGL tracer asks of the desc beyond what the HLSL one does:
/// `frame`, `background` and `lights` are read as storage buffers, so they need `sg::buffer_usage::readonly_buffer`.
/// Every instance must be a triangle one, since the tracer's structure is `.triangles` until the quadric hit group is ported.
class sv::sgl_pathtrace_routine : public sg::render_routine<sgl_pathtrace_routine>
{
public:
    /// Builds the TLAS from `d.instances`, binds the scene, and integrates one path bundle per pixel into `d.output`.
    ///
    /// Declines, leaving the target untouched, while its hit group or its pipeline is still building and after either failed.
    /// The pipeline is built over the manager's own bindless layout, which only a trace knows, so the first trace of a layout declines.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd, pt_trace_desc const& d);

    /// The SGL source of the hit group every instance shades with: sv's fallback material, through `tracer.shade_triangle`.
    [[nodiscard]] static cc::string_view fallback_hit_group_source();

protected:
    /// Creates the guide stand-ins, which outlive every shader reload.
    cc::shared_async<cc::unit> init_once(sg::routine_init_scope scope) override;

    /// Compiles the host's hit group and describes the pipeline over it.
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    /// What a target the trace writes nothing to is bound to, since every binding of the group must be filled.
    sg::texture_2d _guide_normal_stand_in;
    sg::texture_2d _guide_depth_stand_in;
    sg::texture_2d _guide_albedo_stand_in;
    sg::texture_2d _guide_specular_albedo_stand_in;
    sg::texture_2d _guide_roughness_stand_in;
    sg::texture_2d _frame_output_stand_in;
    sg::texture_2d _frame_diffuse_stand_in;
    sg::texture_2d _frame_specular_stand_in;
    sg::texture_2d _guide_hit_distance_stand_in;
    sg::texture_2d _guide_motion_stand_in;
    sg::texture_2d _primary_depth_stand_in;

    /// The hit group the host hands the pipeline, and the description slib states over it; empty while init has not settled.
    slib::raytracing_host_parts _host;
    sg::raytracing_pipeline_description _description;
    bool _is_described = false;

    /// One pipeline, built over one bindless layout.
    struct pipeline_variant
    {
        sg::raytracing_pipeline_handle pipeline;
        sg::raytracing_shader_table_handle table;

        /// What every instance's `hit_group_offset` becomes: the row of the one host hit group.
        u32 hit_group_offset = 0;

        /// The state object while it is still being built, polled rather than waited on.
        sg::async_raytracing_pipeline pending;

        /// Set when the state object refused, which the same inputs repeat until a reload.
        bool failed = false;
    };

    /// The variant for `bindless_layout`, or null while it is building and after it failed; never waits.
    [[nodiscard]] pipeline_variant const* _variant_for(sg::context& ctx,
                                                       sg::binding_group_layout_handle const& bindless_layout);

    /// Keyed on the bindless layout's structural hash; dropped by every reload.
    cc::map<cc::hash128, pipeline_variant> _variants;
};
