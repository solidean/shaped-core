#pragma once

#include <clean-core/bytes/hash128.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/raytracing/acceleration_structure.hh> // sg::tlas_instance
#include <shaped-graphics/raytracing/raytracing_pipeline.hh>
#include <shaped-graphics/raytracing/raytracing_shader_table.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-shader-library/raytracing_pipeline.hh> // slib::raytracing_host_parts
#include <shaped-viewer/drawing/decal.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/rendering/stand_in_report.hh>
#include <shaped-viewer/rendering/traced_group.hh>
#include <shaped-viewer/resources/instance_data.hh>
#include <shaped-viewer/scene/background.hh>
#include <shaped-viewer/scene/light.hh>
#include <shaped-viewer/view/camera.hh>
#include <typed-geometry/linalg/pos.hh>

namespace sv
{
/// The frame block a trace starts from, module `tracer`'s `frame_constants`: 16 samples a pixel, 5 bounces, seed 1, and
/// every flag, count and pad zero.
///
/// The camera, the sample controls, and the table that indexes `pt_trace_desc::lights`: how many there are, and where each
/// path's run starts in a buffer grouped by path.
/// `pt_light_table::describe_in` is what fills that table, so it cannot disagree with the buffer it describes.
/// `accum_frame` is how many frames the target's running mean already holds: 0 overwrites the target, anything above it
/// blends in place at 1 / (accum_frame + 1), and nothing caps it, so the caller restarts it whenever the image stopped
/// describing what this frame renders.
[[nodiscard]] shaders::tracer::frame_constants default_frame_constants();

/// Writes into `record` what `gpu_resource_manager::describe_instance` leaves to the trace: the placement, and how its cutout opacity is read.
///
/// The placement is copied from `instance`, the TLAS instance the record describes, so the two cannot disagree.
/// `opacity` is the permutation the instance shades with THIS trace, which only the trace knows: a stand-in's cuts out nothing.
void place_instance_record(shaders::tracer::instance_record& record,
                           sg::tlas_instance const& instance,
                           material_opacity const& opacity);

static_assert(u32(light_path::distant_disc) == 3, "the frame block's path table has one slot per light_path");

/// The path of an emissive instance's light record, `scene.light_path_mesh`: one past every `light_path`, since no `sv::light` takes it.
/// Its records stand after every analytic one, from `frame_constants::emitter_offset`.
inline constexpr u32 light_path_mesh = 4;

/// What an instance record's `emitter` holds where the instance lights nothing, `scene.no_offset`.
inline constexpr u32 no_emitter = 0xffffffffu;

/// What next-event estimation reads of an emissive instance beyond its light record, `traced.emitters`' element.
///
/// `instance` is the instance's row of the instance table, and `cdf` the bindless index of its mesh's area CDF over `triangle_count` triangles in `block_count` blocks.
/// `emission` is the permutation the instance shades with, whose factors `tracer.emitter_radiance` reads at the sampled point.
[[nodiscard]] shaders::tracer::emitter_record emitter_record_of(u32 instance,
                                                                u32 cdf,
                                                                u32 triangle_count,
                                                                u32 block_count,
                                                                material_emission const& emission);
} // namespace sv

/// How next-event estimation picks among a table's lights.
enum class sv::light_selection : sv::u8
{
    /// Half in proportion to each light's power, and half uniformly over the lights with light to offer.
    /// Within the power half the distant lights, which have no power, share it half and half with the local ones where both have light.
    /// The uniform half is what keeps a dim light near a shading point from being starved, since power knows nothing of distance.
    power,
    /// Every light alike, which the records say by leaving their selection probabilities 0.
    uniform,
};

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
    /// Grouped by path in `light_path` order, each run keeping the order the lights were given in.
    /// The emissive instances' records are last.
    cc::vector<shaders::tracer::light_record> records;
    /// Indexed by path, `light_path_mesh` included; the frame block carries the first four, and the fifth's offset as `emitter_offset`.
    u32 path_offset[5] = {};
    u32 path_count[5] = {};

    /// Also writes each record's selection probability and the cumulative one, in the order the table keeps, or 0 in both for `light_selection::uniform`.
    [[nodiscard]] static pt_light_table grouped(cc::span<shaders::tracer::light_record const> lights,
                                                light_selection selection = light_selection::power);

    /// Writes the count, the per-path table and where the emissive instances' records start into `fc`.
    void describe_in(shaders::tracer::frame_constants& fc) const;

    /// A hash of the records, of `emitters`, the `traced.emitters` this table's mesh records name, in their order,
    /// and of where each emitter's row of `instances`, the `traced.instances`, places it.
    /// What ReSTIR DI keeps its history under (`wavefront_trace_desc::lights_key`): a reservoir names a light by its index and a triangle by its primitive,
    /// so any change to what an index means drops it.
    /// An emissive instance that moves, rigidly or not, changes it too, so a moved emitter drops every DI history, as the view's scene hash does.
    /// Left out is what says how candidates are drawn rather than what a kept sample is: every record's `select_pdf` and `select_cdf`,
    /// a mesh record's `area`, `emission` and `light_flag_unseen_emission`, which selection alone reads, and each emitter's `cdf`, whose bindless index is epoch-scoped.
    /// Nothing of the camera is in it, nor the placement of an instance that lights nothing.
    /// Nor an emitter's geometry: a mesh replaced under the same row, placement and triangle count is the caller's to reset, as the view's scene hash does.
    [[nodiscard]] u64 key(cc::span<shaders::tracer::emitter_record const> emitters,
                          cc::span<shaders::tracer::instance_record const> instances) const;
};

/// Everything one view's path trace binds.
struct sv::pt_trace_desc
{
    /// One element, the camera, the sample controls and the light table; read as a storage buffer, so `readonly_buffer` usage.
    sg::buffer<shaders::tracer::frame_constants> frame;

    /// The SH environment probe the miss reads; read as a storage buffer, so `readonly_buffer` usage.
    sg::buffer<background_gpu> background;

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

    /// The specular half, which the vendor members ask for separately: normal-incidence specular reflectance
    /// (rgba16_float, rgb) and the sharpest specular lobe's perceptual roughness (r16_float).
    ///
    /// Both or neither, at `output`'s extent, and set exactly when the frame block's `write_specular_guides` is.
    /// They blend on `guide_frame` like the three above, since they describe the same image.
    sg::texture_2d guide_specular_albedo;
    sg::texture_2d guide_roughness;

    /// What a temporal denoiser reads: this frame's samples alone (rgba16_float), and each pixel's motion since the previous
    /// frame, as this pixel minus that one in pixels (rg32_float).
    ///
    /// Both or neither, at `output`'s extent, and set exactly when the frame block's `write_temporal` is.
    sg::texture_2d frame_output;
    sg::texture_2d guide_motion;

    /// This frame's samples split the way a split-signal denoiser filters them, plus the first secondary hit distance
    /// of each kind (rg32_float: diffuse in r, specular in g).
    ///
    /// All three or none, at `output`'s extent, and set exactly when the frame block's `write_split` is.
    /// `frame_diffuse` and `frame_specular` sum to `frame_output` exactly, which is what lets a member filter them
    /// apart and add them back without changing the picture.
    sg::texture_2d frame_diffuse;
    sg::texture_2d frame_specular;
    sg::texture_2d guide_hit_distance;

    /// The first sample's primary-hit clip depth per pixel (r32_float), at `output`'s extent, or null.
    /// Set exactly when the frame block's `write_primary_depth` is; what a raster pass after the trace is occluded by.
    sg::texture_2d primary_depth;

    /// How many samples each pixel of `output` averages (r32_float), at `output`'s extent, or null.
    /// Only the wavefront tracer writes it, and only while the guides are written: its pixels average different counts, which the megakernel's never do.
    sg::texture_2d sample_counts;

    /// One `sv::shaders::tracer::instance_record` per entry of `instances`, in that same order — `traced.instances`, read by a hit's instance id.
    /// Everything a hit needs is reached from here, which is what lets one view hold any number of meshes and materials.
    sg::buffer<shaders::tracer::instance_record> instance_table;

    /// Every light the trace samples, grouped by path as `pt_light_table` groups them — `traced.lights`.
    ///
    /// Null means no lights at all, and then the frame block's `light_count` must be 0.
    /// The routine binds a zeroed stand-in for it, since a binding cannot be empty.
    sg::buffer<shaders::tracer::light_record> lights;

    /// One record per light of path `light_path_mesh` in `lights`, in their order — `traced.emitters`.
    /// Null means none, and then `lights` holds no such record; the routine binds a zeroed stand-in.
    /// Every instance record whose `emitter` names one of them must be the row its `emitter_record::instance` names.
    sg::buffer<shaders::tracer::emitter_record> emitters;

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
    /// Which of the two a permutation wants is read off `material_permutation::kind`.
    ///
    /// Null is the same all-or-nothing behavior `fallback` describes, for quadric permutations alone.
    material_permutation const* quadric_fallback = nullptr;

    /// The manager's bindless tables, snapshotted and locked for this recording — bound as the pipeline's second group.
    /// It must outlive the dispatch, which is what `gpu_resource_manager::freeze()`'s scope is for.
    bound_resources const* bindless = nullptr;

    /// The decals every hit is painted with, `frame`'s `decal_count` of them, and the shapes they cover, `traced.decals`
    /// and `traced.decal_shapes`.
    /// Null means none, and the routine binds stand-ins.
    sg::buffer<shaders::tracer::decal_record> decals;
    sg::buffer<shaders::tracer::decal_shape> decal_shapes;

    /// The atlas those shapes live in, already prepared, bound as `slug.tables`, the pipeline's third group.
    /// Null means no decals, and the routine binds empty stand-ins.
    sr::slug_atlas const* decal_atlas = nullptr;

    /// Where set, receives what a frame that executed traced in place of the permutations of `hit_groups`.
    /// A caller accumulating a mean folds its hash into the one its image restarts on, since a different set of shaders is a different image.
    stand_in_report* stand_ins = nullptr;
};

/// The global-illumination path-tracing pass: shaders/tracer_pipeline.sgl, over module `tracer` (shaders/sgl).
///
/// A render routine owning one ray-tracing pipeline per **ordered set of material permutations** a trace binds.
/// The hit groups are generated per material rather than authored, so which shaders a pipeline is built from is a property
/// of the scene and cannot be settled in `init`.
/// Pipelines are cached on that set, so a scene whose materials are stable builds one and rebinds it every frame.
///
/// The bindings come in three groups: group 0 is `tracer.traced`, the trace's own (the TLAS, the targets, the frame block, the
/// instance table, the lights, the decals).
/// Group 1 is the manager's bindless tables, whose layout is `tracer.bindless` itself, so the snapshot binds as it is and sg
/// resolves its footprint by name.
/// Group 2 is `slug.tables`, the atlas the decals' shapes live in, which only a closest hit lists.
///
/// Each permutation's `hit_group` is one row of the shader table, a record per ray type.
/// A caller's `hit_group_offset` counts two records per permutation and is rewritten to that permutation's row.
/// A permutation whose hit group has not compiled is substituted by `fallback` or `quadric_fallback`, by its kind.
///
/// The raygen bounces each ray and estimates direct light at every hit by next-event estimation toward two sources:
/// one light, picked half by power and half uniformly (`pt_light_table`), and the SH environment.
/// **Both are gathered by balance-heuristic multiple importance sampling** against the BSDF-sampled bounce ray.
/// The environment pairs with that ray escaping, a light with it crossing that light, which is analytic and so is intersected
/// rather than traced.
/// A bounce ray next-event estimation could not have drawn, one that transmitted or left a medium scatter, takes the full weight.
/// The light half is what keeps a near-smooth surface usable: light sampling alone has to carry the whole GGX peak there.
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
    /// So this declines, leaving the target untouched, while a hit group, the pipeline's description or the pipeline itself
    /// is still building, and after either failed.
    ///
    /// Declining is what a caller must look at rather than infer.
    /// Degrading silently is right for a live reload and wrong for a test, where a broken shader would otherwise leave
    /// an untouched target no CPU-side assertion notices — so the outcome is nodiscard and the tests assert on it.
    [[nodiscard]] static sg::routine_outcome execute(sg::command_list& cmd, pt_trace_desc const& d);

protected:
    /// Creates the guide stand-ins, which outlive every shader reload.
    cc::shared_async<cc::unit> init_once(sg::routine_init_scope scope) override;

    /// Drops every pipeline, which a reload makes stale.
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    /// What a target the trace writes nothing to is bound to, the wavefront's path state included.
    impl::traced_stand_ins _stand_ins;

    /// One pipeline, built over one ordered set of hit groups, in two steps polled rather than waited on.
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
    /// `stand_ins` receives what it stands in with a fallback.
    [[nodiscard]] pipeline_variant const* _variant_for(sg::context& ctx,
                                                       pt_trace_desc const& d,
                                                       stand_in_report& stand_ins);

    /// Keyed on the substituted hit groups in order; dropped by every reload.
    /// A map rather than a vector for the references: a variant is held across the dispatch that follows its build.
    cc::map<cc::hash128, pipeline_variant> _variants;
};
