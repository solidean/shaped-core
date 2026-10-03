#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/vector.hh>
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
/// **Its hit groups are the desc's permutations**, each one's `material_permutation::sgl_hit_group` rather than its HLSL shaders.
/// A permutation whose SGL group has not compiled is substituted by `fallback` or `quadric_fallback`, by its kind, as `pathtrace_routine` substitutes.
/// Each instance's `hit_group_offset` is read as `pathtrace_routine` reads it, two records per permutation, and rewritten to that permutation's row.
///
/// What the SGL tracer asks of the desc beyond what the HLSL one does:
/// `frame`, `background` and `lights` are read as storage buffers, so they need `sg::buffer_usage::readonly_buffer`.
class sv::sgl_pathtrace_routine : public sg::render_routine<sgl_pathtrace_routine>
{
public:
    /// Builds the TLAS from `d.instances`, binds the scene, and integrates one path bundle per pixel into `d.output`.
    ///
    /// Declines, leaving the target untouched, while a hit group, the description or the pipeline is still building, and after either failed.
    /// The pipeline is keyed on the substituted hit groups in order and on the manager's bindless layout, which only a trace knows.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd, pt_trace_desc const& d);

protected:
    /// Creates the guide stand-ins, which outlive every shader reload.
    cc::shared_async<cc::unit> init_once(sg::routine_init_scope scope) override;

    /// Drops every pipeline, which a reload makes stale.
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

    /// One pipeline, built over one ordered set of hit groups and one bindless layout, in two steps polled rather than waited on.
    struct pipeline_variant
    {
        /// The hit groups the host hands the pipeline, in the desc's order, which the table's rows follow.
        slib::raytracing_host_parts host;

        /// slib's description over `host`, while it is still being stated.
        cc::shared_async<sg::raytracing_pipeline_description> pending_description;

        /// The state object while it is still being built.
        sg::async_raytracing_pipeline pending;

        sg::raytracing_pipeline_handle pipeline;
        sg::raytracing_shader_table_handle table;

        /// What an instance's `hit_group_offset` becomes, per hit group: the first record of its row.
        cc::vector<u32> row_offsets;

        /// Set when the description or the state object refused, which the same inputs repeat until a reload.
        bool failed = false;
    };

    /// The variant for `d`'s hit groups, or null while it is building and after it failed; never waits.
    [[nodiscard]] pipeline_variant const* _variant_for(sg::context& ctx, pt_trace_desc const& d);

    /// Keyed on the substituted hit groups in order and the bindless layout's structural hash; dropped by every reload.
    cc::map<cc::hash128, pipeline_variant> _variants;
};
