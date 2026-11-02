#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec.hh>

// The SGL passes both MetalFX members run around Apple's scaler: metalfx_prepare.sgl before it, which writes sr's
// guides in the forms the scaler reads, and metalfx_copy_out.sgl after it, for an output the scaler cannot render to.
// Shared by sr::metalfx_upscale_routine and sr::metalfx_rr_routine, which each build their own copy at init.

namespace sr::impl
{
/// The near plane metalfx_prepare.sgl divides by.
/// Any positive value: the upscaler only compares depths, and `metalfx_view_to_clip` matches the denoised scaler's projection to it.
/// A surface nearer than it gets a depth past 1.
inline constexpr f32 metalfx_near_plane = 0.1f;

/// The compiled passes.
struct metalfx_passes
{
    /// metalfx_copy_out.sgl compiled for one output format.
    struct copy_pass
    {
        sg::pixel_format format = sg::pixel_format::undefined;
        sg::binding_group_layout_handle layout;
        sg::compute_pipeline_handle pipeline;
    };

    sg::binding_group_layout_handle prepare_layout;
    sg::compute_pipeline_handle prepare;
    cc::vector<copy_pass> copies;

    /// The copy into `format`, or null where there is none: an output in that format must need no staging.
    [[nodiscard]] copy_pass const* copy_to(sg::pixel_format format) const;
};

/// One stream's images around the scaler.
struct metalfx_images
{
    /// metalfx_prepare.sgl's output, at the input extent.
    sg::texture_2d depth;
    sg::texture_2d motion;
    sg::texture_2d exposure;

    /// What MetalFX writes when the caller's output needs staging; empty otherwise.
    sg::texture_2d staged;
};

/// Builds `out`; false, logged, when a pass does not build.
[[nodiscard]] cc::shared_async<bool> metalfx_build_passes(sg::context& ctx, metalfx_passes& out);

/// Records metalfx_prepare.sgl: sr's linear `depth` and `motion` into `images`, and `exposure` into its texel.
void metalfx_record_prepare(sg::command_list& cmd,
                            metalfx_passes const& passes,
                            metalfx_images& images,
                            sg::texture_2d const& depth,
                            sg::texture_2d const& motion,
                            f32 exposure);

/// How MetalFX's result reaches an output.
enum class metalfx_output_path
{
    direct, ///< MetalFX writes the output itself: it has `image` and `render_target`
    copy,   ///< staged, then metalfx_copy_out.sgl writes it as an image: it has `image` and a format the copy has
    blit,   ///< staged, then `sr::blit_routine` draws it in: it has `render_target`, in any renderable format
    none,   ///< nothing can write it
};

/// The path to an output, and the format MetalFX writes, which is what its scaler must be created for.
struct metalfx_output_plan
{
    metalfx_output_path path = metalfx_output_path::none;

    /// The output's own, except on the blit path for a format other than rgba16_float or rgba32_float, which stages in
    /// rgba16_float: the output is then typically sRGB, which no image may be.
    sg::pixel_format format = sg::pixel_format::undefined;
};

/// The plan for `output`, given the copies `passes` has.
[[nodiscard]] metalfx_output_plan metalfx_output_plan_of(metalfx_passes const& passes, sg::texture_2d const& output);

/// The image the scaler writes: `output` itself on the direct path, otherwise `images.staged` at the output's extent and
/// `plan.format`.
[[nodiscard]] sg::texture_2d metalfx_target(sg::context& ctx,
                                            metalfx_images& images,
                                            sg::texture_2d const& output,
                                            metalfx_output_plan const& plan);

/// Draws `images.staged` into `output` with `sr::blit_routine`, which the caller found ready before recording anything.
/// False when it declines anyway, its pipeline having failed to build, and then nothing was drawn.
[[nodiscard]] bool metalfx_record_blit_out(sg::command_list& cmd,
                                           metalfx_images const& images,
                                           sg::texture_2d const& output);

/// Records the copy from `images.staged` into `output`.
void metalfx_record_copy_out(sg::command_list& cmd,
                             metalfx_passes::copy_pass const& copy,
                             metalfx_images const& images,
                             sg::texture_2d const& output);

/// The projection that matches metalfx_prepare.sgl's depth: `view_to_clip`'s x and y rows, with a reversed infinite
/// depth of `metalfx_near_plane / depth` along whichever view axis `view_to_clip` looks down.
[[nodiscard]] tg::mat4f metalfx_view_to_clip(tg::mat4f const& view_to_clip);
} // namespace sr::impl
