#pragma once

#include <sgl_modules/tracer.hh> // sv::shaders::tracer::traced and the wavefront's records
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// The wavefront tracer's path state for one view: a slot per pixel, persistent across frames, since a path may outlive one.
///
/// Every buffer is `tracer.traced`'s member of the same name, and libs/graphics/shaped-viewer/docs/wavefront.md says what each holds.
/// `args` is the one exception, `tracer.dispatch`'s, which only the kernels counting a queue bind.
/// The state belongs to the view rather than to a frame, so a caller keeps it as long as the view's extent holds,
/// and a new one starts every path afresh.
struct sv::wavefront_state
{
    tg::vec2i extent = tg::vec2i(0, 0);

    /// How many shading permutations a trace through this state may name, which sizes the counters and the arguments.
    i32 max_permutations = 0;

    sg::buffer<shaders::tracer::path_ray> path_rays;
    sg::buffer<shaders::tracer::path_state> path_states;
    sg::buffer<shaders::tracer::path_medium> path_media;
    sg::buffer<shaders::tracer::path_light> path_lights;
    sg::buffer<shaders::tracer::hit_record> hits;
    sg::buffer<shaders::tracer::shadow_ray> shadow_rays;
    sg::buffer<shaders::tracer::pixel_tally> tallies;
    sg::buffer<u32> queue_a;
    sg::buffer<u32> queue_b;
    sg::buffer<u32> queue_hits;
    sg::buffer<u32> queue_sorted;
    sg::buffer<u32> queue_shadow;
    sg::buffer<u32> queue_finished;
    sg::buffer<u32> counters;
    sg::buffer<u32> shade_offsets;
    sg::buffer<shaders::tracer::dispatch_args> args;

    /// The camera generation the last restart started its paths under, which `wavefront_routine::execute` advances on every restart.
    /// 0 until the first, which is how a fresh state is told from one that has run.
    u32 generation = 0;

    /// A state for `extent`, every path dead and every tally empty: the first trace through it has to restart.
    [[nodiscard]] static wavefront_state create(sg::context& ctx, tg::vec2i extent, i32 max_permutations);

    /// What every buffer of the state occupies together.
    [[nodiscard]] isize size_in_bytes() const;
};

namespace sv::impl
{
/// What a trace binds where it writes nothing, since every binding of `tracer.traced` must be filled.
/// Shared by both tracers: the megakernel binds `paths` as one-slot stand-ins it never reads.
struct traced_stand_ins
{
    sg::texture_2d guide_normal;
    sg::texture_2d guide_depth;
    sg::texture_2d guide_albedo;
    sg::texture_2d guide_specular_albedo;
    sg::texture_2d guide_roughness;
    sg::texture_2d frame_output;
    sg::texture_2d frame_diffuse;
    sg::texture_2d frame_specular;
    sg::texture_2d guide_hit_distance;
    sg::texture_2d guide_motion;
    sg::texture_2d primary_depth;
    sg::texture_2d sample_counts;

    /// What a trace without decals binds as the atlas's two tables and the two decal buffers.
    sg::texture_2d decal_curves;
    sg::texture_2d decal_bands;
    sg::buffer<shaders::tracer::decal_record> decals;
    sg::buffer<shaders::tracer::decal_shape> decal_shapes;

    /// What a trace without emissive instances binds as `traced.emitters`.
    sg::buffer<shaders::tracer::emitter_record> emitters;

    /// One slot of path state, for a trace that keeps none.
    wavefront_state paths;

    [[nodiscard]] static traced_stand_ins create(sg::context& ctx);
};

/// `tracer.traced` for `d`, through `tlas` and `lights`, with `paths` as its path state.
/// `lights` replaces `d.lights`, which may be null where `light_count` is 0; `frame` replaces `d.frame` where the caller uploaded its own.
[[nodiscard]] shaders::tracer::traced traced_group_of(pt_trace_desc const& d,
                                                      traced_stand_ins const& s,
                                                      sg::tlas_handle const& tlas,
                                                      sg::buffer<shaders::tracer::light_record> const& lights,
                                                      sg::buffer<shaders::tracer::frame_constants> const& frame,
                                                      wavefront_state const& paths);

/// `sgl_modules::slug::tables` for `d`'s decal atlas, or for the stand-ins where it has none yet.
[[nodiscard]] sg::binding_group_handle decal_tables_of(sg::command_list& cmd,
                                                       pt_trace_desc const& d,
                                                       traced_stand_ins const& s);
} // namespace sv::impl
