#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh> // sg::async_compiled_shader is a cc::shared_async
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh> // sg::tlas_instance
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/resources/instance_data.hh>
#include <shaped-viewer/scene/background.hh>
#include <shaped-viewer/scene/light.hh> // light_gpu
#include <shaped-viewer/view/camera.hh> // camera_gpu
#include <typed-geometry/linalg/pos.hh>

/// The per-view constant block the path tracer reads at b0 (the FrameConstants struct in shaders/pt_common.hlsli).
/// Mirrors it lane-for-lane — keep them in lockstep.
///
/// The camera, the sample controls, and the table that indexes `pt_trace_desc::lights`: how many there are, and where each
/// path's run starts in a buffer grouped by path.
/// `pt_light_table::describe_in` is what fills that table, so it cannot disagree with the buffer it describes.
/// Laid out as 16-byte lanes to match HLSL cbuffer packing, which is why the table is two `uint4`s rather than arrays.
struct sv::pt_frame_constants_gpu
{
    camera_gpu camera;

    i32 samples_per_pixel = 16; // primary rays integrated per pixel, accumulated in the one dispatch
    i32 max_bounces = 5;        // path length: primary hit + this many diffuse bounces
    u32 seed = 1;               // per-frame RNG seed; vary it to decorrelate accumulated frames

    /// How many frames the target's running mean already holds, which is this frame's weight: 1 / (accum_frame + 1).
    ///
    /// 0 overwrites the target, anything above it blends in place, and nothing caps it — the estimate is exact and
    /// converges as long as it is left alone.
    /// The caller restarts it by sending 0, which it does whenever the image the target holds stopped describing
    /// what this frame renders — the scene, the camera or the shaders having moved.
    u32 accum_frame = 0;

    /// How many lights `pt_trace_desc::lights` holds; 0 lights the scene by the environment alone.
    u32 light_count = 0;
    u32 _pad0[3] = {};

    /// Where each `light_path`'s run starts in the grouped buffer, and how long it is, indexed by the path.
    u32 path_offset[4] = {};
    u32 path_count[4] = {};

    /// Whether the raygen writes the denoiser guides — nonzero exactly when `pt_trace_desc` carries guide textures.
    u32 write_guides = 0;

    /// How many frames the guide textures already average; the same 0-overwrites rule as `accum_frame`, on a count of
    /// its own.
    u32 guide_frame = 0;
    /// Whether the raygen writes this frame's own samples and the motion vectors, for a temporal denoiser — nonzero
    /// exactly when `pt_trace_desc` carries `frame_output` and `guide_motion`.
    u32 write_temporal = 0;
    u32 _guide_pad = 0;

    /// The camera this layer's previous frame was traced from, which the motion vectors reproject into.
    /// The current camera when there was none, which reads as no motion.
    camera_gpu previous_camera = {};

    // Pad the block to a full 256-byte CBV range (see frame_constants.hh).
    f32 _reserved[12] = {};
};

namespace sv
{

static_assert(sizeof(pt_frame_constants_gpu) == 256, "pt_frame_constants_gpu must be a full 256-byte CBV block");
static_assert(u32(light_path::distant_disc) == 3, "the frame block's path table has one slot per light_path");

} // namespace sv

/// The lights one trace samples, grouped by path — what `pt_trace_desc::lights` holds and the frame block's table indexes.
///
/// Grouping is what lets a loop over every light of one path run in one branch, which is where the tracer visits lights
/// wholesale: a bounce ray tests every area light it could have reached.
/// It is a counting sort over the lights the flatten walks anyway, so it costs nothing a frame did not already spend.
///
/// The buffer's order is this table's, never the caller's: a `light_ref` indexes the layer's list, and nothing may
/// carry an index across from one to the other.
struct sv::pt_light_table
{
    /// grouped by path in `light_path` order, each run keeping the order the lights were given in
    cc::vector<light_gpu> records;
    u32 path_offset[4] = {};
    u32 path_count[4] = {};

    [[nodiscard]] static pt_light_table grouped(cc::span<light_gpu const> lights);

    /// Writes the count and the per-path table into `fc`.
    void describe_in(pt_frame_constants_gpu& fc) const;
};

/// Everything one view's path trace binds.
/// Mirrors trace_desc, but the frame block is a pt_frame_constants_gpu — it carries the sample controls and the light table the integrator needs.
struct sv::pt_trace_desc
{
    sg::buffer<pt_frame_constants_gpu> frame;    // the FrameConstants cbuffer (camera + sample controls + light table)
    sg::buffer<background_gpu> background;       // the Background cbuffer (SH environment probe) the miss reads
    cc::span<sg::tlas_instance const> instances; // one per scene item; the TLAS is (re)built from these

    /// The accumulator the raygen blends into: read back and rewritten at the dispatch's own pixel.
    ///
    /// rgba32_float, and that is a requirement rather than a preference.
    /// The blend weight is 1 / (`accum_frame` + 1), so a half float stops moving the mean a couple of thousand
    /// frames in — which is exactly where an uncapped estimate is supposed to still be converging.
    sg::texture_2d output;

    /// The denoiser guides the raygen blends into beside `output`, all zero where the primary ray escaped: the primary
    /// hit's facing normal (rgba16_float, rgb), linear view depth (r32_float) and diffuse albedo (rgba16_float, rgb).
    ///
    /// All three or none, at `output`'s extent, and set exactly when the frame block's `write_guides` is.
    /// Left empty, the trace binds 1x1 stand-ins of its own that nothing writes.
    sg::texture_2d guide_normal;
    sg::texture_2d guide_depth;
    sg::texture_2d guide_albedo;

    /// What a temporal denoiser reads: this frame's samples alone (rgba16_float), and each pixel's motion since the previous
    /// frame, as this pixel minus that one in pixels (rg32_float).
    ///
    /// Both or neither, at `output`'s extent, and set exactly when the frame block's `write_temporal` is.
    sg::texture_2d frame_output;
    sg::texture_2d guide_motion;

    /// One `sv::instance_gpu` per entry of `instances`, in that same order — the closest-hit's `Instances`, read by `InstanceID()`.
    /// Everything a hit needs is reached from here, which is what lets one view hold any number of meshes and materials.
    sg::buffer<instance_gpu> instance_table;

    /// Every light the trace samples, grouped by path as `pt_light_table` groups them — the shaders' `Lights`.
    ///
    /// Null means no lights at all, and then the frame block's `light_count` must be 0.
    /// The routine binds a zeroed stand-in for it, since a binding cannot be empty.
    sg::buffer<light_gpu> lights;

    /// The permutations this trace's instances shade with, one hit group each, in hit-group index order.
    ///
    /// `sg::tlas_instance::hit_group_offset` indexes exactly this list, so the caller has already fixed the order and
    /// must not disturb it between building the instances and getting here.
    /// A permutation whose shader has not compiled — still in flight, or a broken material — is replaced by
    /// `fallback` or `quadric_fallback` for this trace, so one bad material costs its own meshes their shading rather
    /// than costing the view its whole image.
    cc::span<material_permutation const* const> hit_groups;

    /// The neutral TRIANGLE hit group a permutation that did not compile is substituted by — `material_shader_cache::acquire_fallback`.
    ///
    /// Null means no substitution: a permutation that has not compiled then makes the whole trace a no-op, which is the
    /// old all-or-nothing behavior and what a caller with no cache at hand gets.
    material_permutation const* fallback = nullptr;

    /// The neutral PROCEDURAL hit group, for a quadric permutation — `material_shader_cache::acquire_quadric_fallback`.
    ///
    /// **A substitution has to keep the hit group's kind**, and that is a correctness requirement rather than tidiness:
    /// a procedural BLAS must be traced by a group carrying an intersection shader, so standing a quadric permutation
    /// in with the triangle fallback reports no hits at all and the batch silently disappears until its compile lands.
    /// Which of the two a permutation wants is read off `material_permutation::intersection`.
    ///
    /// Null is the same all-or-nothing behavior `fallback` describes, for quadric permutations alone.
    material_permutation const* quadric_fallback = nullptr;

    /// The manager's bindless tables, snapshotted and locked for this recording — bound as the pipeline's second group.
    /// It must outlive the dispatch, which is what `gpu_resource_manager::freeze()`'s scope is for.
    bound_resources const* bindless = nullptr;
};

/// The global-illumination path-tracing pass.
///
/// A render routine, structured exactly like pbr_raytrace_routine.
/// It owns the slib-acquired raygen and miss shaders, and one DXR pipeline per **set of material permutations** a trace binds.
/// The closest-hit is generated per material rather than authored, so which shaders a pipeline is built from is a property of the scene and cannot be settled in `init`.
/// What can, and is, are the three shaders every pipeline shares.
/// Pipelines are cached on that set, so a scene whose materials are stable builds one and rebinds it every frame.
///
/// The bindings come in two groups, and that split is the reflection's rather than a convenience:
/// group 0 is the trace's own (the TLAS, the targets, the constants, the instance table, the lights).
/// Group 1 is the manager's bindless tables, which sv owns as a schema and no shader gets to redeclare.
/// Where the tracer shades a surface is the generated hit group; how it integrates is `shaders/pathtrace.hlsl`, which is shared.
/// The raygen bounces each ray diffusely and estimates direct light at every hit by next-event estimation toward two sources: one light, picked uniformly from the trace's, and the SH environment.
/// **Both are gathered by balance-heuristic multiple importance sampling** against the BSDF-sampled bounce ray.
/// The environment pairs with that ray escaping, a light with it crossing that light, which is analytic and so is intersected rather than traced.
/// Lights are analytic and occlude nothing, so the bounce ray counts every light it crosses before the surface, each against its own density.
/// The light half is what keeps a near-smooth surface usable.
/// Light sampling alone has to carry the whole GGX peak there — a huge value at a tiny probability, which is a firefly per few thousand samples rather than a converging estimate.
/// `samples_per_pixel` paths per pixel accumulate in one dispatch.
/// `execute` may build a pipeline, so it takes the exclusive acquire — two traces on one context serialize on this routine.
class sv::pathtrace_routine : public sg::render_routine<pathtrace_routine>
{
public:
    /// Builds the TLAS from `d.instances`, binds the scene, and integrates one path bundle per pixel over `d.output`'s extent into `d.output`.
    ///
    /// **Fallible, and for a reason the other routines do not share.**
    /// Its pipelines are keyed on the ordered set of hit groups a trace names, which is scene data: unbounded, and
    /// discovered on the frame path when a material combination is first used.
    /// That key cannot be a routine parameter, so the permutations stay a map, and this declines and leaves the target
    /// untouched until the one this trace needs has been built.
    ///
    /// Declining is what a caller must look at rather than infer.
    /// Degrading silently is right for a live reload and wrong for a test, where a broken shader would otherwise leave
    /// an untouched target no CPU-side assertion notices — so the outcome is nodiscard and the tests assert on it.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd, pt_trace_desc const& d);

protected:
    /// Creates the guide stand-ins, which outlive every shader reload.
    cc::shared_async<cc::unit> init_once(sg::routine_init_scope scope) override;

    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    /// What the guide bindings hold when a trace writes no guides: every binding of the group must be filled, and the
    /// raygen never writes them while `write_guides` is clear.
    sg::texture_2d _guide_normal_stand_in;
    sg::texture_2d _guide_depth_stand_in;
    sg::texture_2d _guide_albedo_stand_in;
    sg::texture_2d _frame_output_stand_in;
    sg::texture_2d _guide_motion_stand_in;

    /// One pipeline, built over one ordered set of hit groups.
    ///
    /// `group_layout` covers the trace's own bindings alone: the manager's tables are the second group and are
    /// deliberately not merged into it, so a generated shader redeclaring them cannot change sv's schema.
    /// The pipeline layout is not among these — the pipeline holds it, which is what keeps the root signature alive.
    struct pipeline_variant
    {
        sg::binding_group_layout_handle group_layout;
        sg::raytracing_pipeline_handle pipeline;
        sg::raytracing_shader_table_handle table;
        sg::raygen_index raygen = {};

        /// The state object while it is still being built.
        /// Held rather than waited on: this permutation is discovered on the frame path, and a build there is the one
        /// thing that must not stall — so the frames until it lands trace without it.
        sg::async_raytracing_pipeline pending;

        /// Set when this permutation cannot be built: a shader that will not compile, or a state object that refused.
        /// Remembered rather than retried every frame, since the same inputs fail the same way until a reload.
        bool failed = false;

        /// What the shader table is built from, kept until the pipeline it indexes into exists.
        /// These are positions in the pipeline description rather than objects, so holding them costs nothing.
        sg::raygen_shader_handle pending_raygen = {};
        sg::miss_shader_handle pending_miss = {};
        sg::miss_shader_handle pending_shadow_miss = {};
        cc::vector<sg::hit_shader_handle> pending_hits;
    };

    /// Builds the shader table for a variant whose pipeline has just landed.
    static void _finish_variant(sg::context& ctx, pipeline_variant& variant);

    /// The variant for `d`'s hit groups, or null while it is still being built or after it failed.
    ///
    /// Never waits.
    /// A permutation is discovered when a frame first uses that material set, which is on the frame path — so this
    /// starts the work and reports what is ready, and the trace happens a frame or two later.
    [[nodiscard]] pipeline_variant const* _variant_for(sg::context& ctx, pt_trace_desc const& d);

    // Re-acquired by init on every reload, which is also when every variant built from the old ones is dropped.
    sg::async_compiled_shader _raygen_shader;
    sg::async_compiled_shader _miss_shader;
    sg::async_compiled_shader _shadow_miss_shader;

    /// Keyed on the hit-group set in order, together with the layout the second group is bound through.
    /// A map rather than a vector for the references: a variant is held across the dispatch that follows its build.
    cc::map<cc::hash128, pipeline_variant> _variants;
};
