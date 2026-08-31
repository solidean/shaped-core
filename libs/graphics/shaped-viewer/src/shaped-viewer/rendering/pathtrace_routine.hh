#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
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
#include <shaped-viewer/scene/light.hh> // area_light_gpu
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

    /// How many frames the target's running mean already holds, which is this frame's weight: 1 / (accum_frame + 1).
    ///
    /// 0 overwrites the target, anything above it blends in place, and nothing caps it — the estimate is exact and
    /// converges as long as it is left alone.
    /// The caller restarts it by sending 0, which it does whenever the image the target holds stopped describing
    /// what this frame renders — the scene, the camera or the shaders having moved.
    u32 accum_frame = 0;

    // Pad the block to a full 256-byte CBV range (see frame_constants.hh).
    f32 _reserved[24] = {};
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

    /// The accumulator the raygen blends into: read back and rewritten at the dispatch's own pixel.
    ///
    /// rgba32_float, and that is a requirement rather than a preference.
    /// The blend weight is 1 / (`accum_frame` + 1), so a half float stops moving the mean a couple of thousand
    /// frames in — which is exactly where an uncapped estimate is supposed to still be converging.
    sg::texture_2d output;

    /// One `sv::instance_gpu` per entry of `instances`, in that same order — the closest-hit's `Instances`, read by `InstanceID()`.
    /// Everything a hit needs is reached from here, which is what lets one view hold any number of meshes and materials.
    sg::buffer<instance_gpu> instance_table;

    /// The permutations this trace's instances shade with, one hit group each, in hit-group index order.
    ///
    /// `sg::tlas_instance::hit_group_offset` indexes exactly this list, so the caller has already fixed the order and
    /// must not disturb it between building the instances and getting here.
    /// A permutation whose shader has not compiled — still in flight, or a broken material — is replaced by
    /// `fallback` for this trace, so one bad material costs its own meshes their shading rather than costing the view
    /// its whole image.
    cc::span<material_permutation const* const> hit_groups;

    /// The neutral hit group a permutation that did not compile is substituted by — `material_shader_cache::acquire_fallback`.
    ///
    /// Null means no substitution: a permutation that has not compiled then makes the whole trace a no-op, which is the
    /// old all-or-nothing behavior and what a caller with no cache at hand gets.
    material_permutation const* fallback = nullptr;

    /// The manager's bindless tables, snapshotted and locked for this recording — bound as the pipeline's second group.
    /// It must outlive the dispatch, which is what `gpu_resource_manager::freeze()`'s scope is for.
    bound_resources const* bindless = nullptr;
};

/// The global-illumination path-tracing pass.
///
/// A render routine, structured exactly like pbr_raytrace_routine.
/// It owns the slib-acquired raygen and miss shaders, and one DXR pipeline per **set of material permutations** a trace binds.
/// The closest-hit is generated per material rather than authored, so which shaders a pipeline is built from is a property of the scene and cannot be settled in `init_declare`.
/// What can, and is, are the three shaders every pipeline shares.
/// Pipelines are cached on that set, so a scene whose materials are stable builds one and rebinds it every frame.
///
/// The bindings come in two groups, and that split is the reflection's rather than a convenience:
/// group 0 is the trace's own (the TLAS, the targets, the constants, the instance table), group 1 is the manager's bindless tables, which sv owns as a schema and no shader gets to redeclare.
/// Where the tracer shades a surface is the generated hit group; how it integrates is `shaders/pathtrace.hlsl`, which is shared.
/// The raygen bounces each ray diffusely and estimates direct light at every hit by next-event estimation toward two sources: the rectangular area light and the SH environment.
/// **Both are gathered by balance-heuristic multiple importance sampling** against the BSDF-sampled bounce ray.
/// The environment pairs with that ray escaping, the light with it crossing the rect, which is analytic and so is intersected rather than traced.
/// The light half is what keeps a near-smooth surface usable.
/// Light sampling alone has to carry the whole GGX peak there — a huge value at a tiny probability, which is a firefly per few thousand samples rather than a converging estimate.
/// `samples_per_pixel` paths per pixel accumulate in one dispatch.
/// `execute` may build a pipeline, so it takes the exclusive acquire — two traces on one context serialize on this routine.
class sv::pathtrace_routine : public sg::render_routine<pathtrace_routine>
{
public:
    /// Builds the TLAS from `d.instances`, binds the scene, and integrates one path bundle per pixel over `d.output`'s extent into `d.output`.
    /// A no-op (leaves the target untouched) if the shaders did not compile, or if any permutation `d` names has not.
    static void execute(sg::command_list& cmd, pt_trace_desc const& d);

    /// Whether the most recent `execute` on this context actually dispatched.
    ///
    /// It reports the *last trace* rather than the routine, because there is no longer one pipeline to ask about: a
    /// pipeline exists per permutation set, so readiness only means anything relative to a trace that named one.
    /// `execute` degrades to a no-op rather than throwing, which is the right behavior for a live reload and the
    /// wrong one for a test: a broken shader then leaves an untouched target that no CPU-side assertion notices.
    /// So a test asserts on this, and a debug overlay can say why the image is empty.
    /// False before the first execute.
    [[nodiscard]] static bool is_ready(sg::command_list& cmd);

protected:
    void init_declare(sg::context& ctx) override;

private:
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
    };

    /// The variant for `d`'s hit groups, built on a miss, or null when something it needs has not compiled.
    [[nodiscard]] pipeline_variant const* _variant_for(sg::context& ctx, pt_trace_desc const& d);

    // Re-acquired by init_declare on every reload, which is also when every variant built from the old ones is dropped.
    sg::async_compiled_shader _raygen_shader;
    sg::async_compiled_shader _miss_shader;
    sg::async_compiled_shader _shadow_miss_shader;

    /// Keyed on the hit-group set in order, together with the layout the second group is bound through.
    /// A map rather than a vector for the references: a variant is held across the dispatch that follows its build.
    cc::map<cc::hash128, pipeline_variant> _variants;

    /// Whether the last `execute` dispatched — what `is_ready` reports.
    bool _traced = false;
};
