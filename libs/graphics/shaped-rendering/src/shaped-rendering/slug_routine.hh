#pragma once

#include <clean-core/common/hash.hh>
#include <clean-core/container/span.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

/// One shape to draw: which shape, where its em space lands on the object's xy plane, and its colour.
/// 68 bytes, read once per quad by the vertex stage; build one with `sr::make_slug_instance`.
struct sr::slug_instance
{
    /// Where one stored em unit along x and along y lands in object space: (x.x, x.y, y.x, y.y).
    tg::vec4f em_to_object;

    /// Where em (0, 0) lands.
    tg::vec2f origin;

    /// The shape's em box, min.xy and max.xy; the quad covers it, widened by half a pixel.
    tg::vec4f em_bounds;

    tg::vec4f banding;
    u32 glyph_location = 0;
    u32 band_info = 0;

    /// rgba8, sRGB-encoded, straight alpha, red in the low byte; the shader linearizes and premultiplies it.
    u32 color = 0xffffffff;
};

/// Where a job places a plane of shapes: the plane's (x, y) lands at `at + x * x_axis + y * y_axis`, in whatever space
/// the draw's `object_to_clip` starts from — pixels for a 2D overlay, the world for a scene.
/// The axes are free: their lengths and angle stretch and shear the plane.
struct sr::slug_frame
{
    tg::pos3f at;
    tg::vec3f x_axis = tg::vec3f(1, 0, 0);
    tg::vec3f y_axis = tg::vec3f(0, 1, 0);

    /// rgba8, sRGB-encoded, straight alpha, red in the low byte; multiplies the colour of every shape under this frame.
    u32 tint = 0xffffffff;
};

/// One quad of a job: the atlas record it draws, under which of the job's frames.
struct sr::slug_quad
{
    u32 record = 0;
    u32 frame = 0;
};

/// What one draw needs beyond its instances.
struct sr::slug_view
{
    /// Takes the instances' object xy plane, at z = 0, to clip space.
    tg::mat4f object_to_clip = tg::mat4f::identity;

    /// Pulls every shape toward the camera by this much of clip w, which is what keeps a shape lying on a surface in front of it.
    /// Zero for a 2D overlay.
    f32 depth_bias = 0.0f;

    /// Takes the square root of coverage, which makes thin shapes optically heavier.
    bool weight_boost = false;
};

/// The routine's parameter: the scope's colour format, and its depth format or undefined for none.
struct sr::slug_pipeline_key
{
    sg::pixel_format color = sg::pixel_format::undefined;
    sg::pixel_format depth = sg::pixel_format::undefined;

    [[nodiscard]] bool operator==(slug_pipeline_key const&) const = default;
    [[nodiscard]] friend u64 hash(slug_pipeline_key const& k) { return cc::make_hash(u64(k.color), u64(k.depth)); }
};

/// Draws Slug shapes — glyphs, icons, any outline of quadratic curves — from a caller-owned atlas.
///
///     auto const prepared = sr::slug_routine::prepare(*cmd, atlas, instances);    // before the scope: uploads
///     auto pass = cmd->raster.render_to({.color_targets = {rt.preserved()}});
///     (void)sr::slug_routine::execute(pass, atlas, prepared, {.object_to_clip = mvp});
///
/// A **job** is the instanced form: shapes kept once as the atlas's records, placed many times by frames.
/// Each quad names a record and a frame, so one draw covers any mix of shapes under any number of frames:
///
///     auto const first = atlas.add_records(arrow_layers).value();                 // once
///     auto const job = sr::slug_routine::prepare_job(*cmd, atlas, frames, quads); // before the scope
///     (void)sr::slug_routine::execute(pass, atlas, job, {.object_to_clip = world_to_clip});
///
/// Output is linear and premultiplied, blended premultiplied over the target.
/// A scope with a depth target draws depth-tested without writing depth, so shapes on a surface layer in draw order.
/// One pipeline per (colour, depth) format pair and draw form, built in the background: execute declines until it is ready.
class sr::slug_routine : public sg::render_routine<slug_routine, slug_pipeline_key>
{
public:
    /// Instances uploaded for one recording, on the list `prepare` was given.
    struct prepared_shapes
    {
        sg::command_list const* command_list = nullptr;
        sg::buffer<slug_instance> instances;
    };

    /// A job's frames and quads uploaded for one recording, on the list `prepare_job` was given.
    struct prepared_job
    {
        sg::command_list const* command_list = nullptr;
        sg::texture_2d frames;
        sg::buffer<slug_quad> quads;
    };

    /// Texels one frame takes in a job's frame texture, and how many frames a row of it holds.
    static constexpr int texels_per_frame = 3;
    static constexpr int frames_per_row = 4096 / texels_per_frame;

    /// Uploads `atlas`'s pending shapes and records, and this job's frames and quads; call it before the scope opens.
    /// Every quad must name a record of `atlas` and a frame of `frames`.
    [[nodiscard]] static prepared_job prepare_job(sg::command_list& cmd,
                                                  slug_atlas& atlas,
                                                  cc::span<slug_frame const> frames,
                                                  cc::span<slug_quad const> quads);

    /// Draws what `prepare_job` uploaded into an open scope on the same list.
    /// `view.object_to_clip` takes the frames' space to clip space.
    [[nodiscard]] static sg::routine_outcome execute(sg::rendering_scope& scope,
                                                     slug_atlas const& atlas,
                                                     prepared_job const& job,
                                                     slug_view const& view);

    /// Uploads `atlas`'s pending shapes and this frame's instances; call it before the rendering scope opens.
    [[nodiscard]] static prepared_shapes prepare(sg::command_list& cmd,
                                                 slug_atlas& atlas,
                                                 cc::span<slug_instance const> instances);

    /// Draws what `prepare` uploaded into an open scope on the same list.
    [[nodiscard]] static sg::routine_outcome execute(sg::rendering_scope& scope,
                                                     slug_atlas const& atlas,
                                                     prepared_shapes const& shapes,
                                                     slug_view const& view);

    /// Draws `count` instances from `first` of a buffer the caller keeps — retained text uploads once and redraws free.
    /// The buffer needs vertex_buffer usage, and `atlas` must have no pending upload: prepare it after its last `add`.
    [[nodiscard]] static sg::routine_outcome execute(sg::rendering_scope& scope,
                                                     slug_atlas const& atlas,
                                                     sg::buffer<slug_instance> const& instances,
                                                     isize first,
                                                     isize count,
                                                     slug_view const& view);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::binding_group_layout_handle _group_layout;
    sg::binding_group_layout_handle _job_group_layout;
    sg::async_raster_pipeline _pipeline;
    sg::async_raster_pipeline _job_pipeline;

    /// The six corners of the unit square every quad is drawn from, two triangles.
    sg::buffer<tg::vec2f> _corners;
};

namespace sr
{
/// An instance drawing `shape` with its outline's origin at `origin`, one outline unit along x on `x_axis` and along y on
/// `y_axis` — all in object space, or in a drawing's plane for a record a job's frames place.
/// `shape` must be drawable: an empty shape — a space — has no instance, so the caller skips it.
/// `srgb_color` is straight-alpha, sRGB-encoded, in [0, 1].
[[nodiscard]] slug_instance make_slug_instance(slug_shape_ref const& shape,
                                               tg::pos2f origin,
                                               tg::vec2f x_axis,
                                               tg::vec2f y_axis,
                                               tg::vec4f srgb_color);

/// `c`'s channels in [0, 1] as rgba8, red in the low byte.
[[nodiscard]] u32 pack_rgba8(tg::vec4f c);
} // namespace sr
