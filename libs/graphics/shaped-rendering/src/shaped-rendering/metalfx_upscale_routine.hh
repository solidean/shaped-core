#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/impl/metalfx_passes.hh>
#include <shaped-rendering/reconstruct.hh>

#include <memory>

/// Apple MetalFX's temporal upscaler, on a Metal context.
///
/// MetalFX encodes its own work into sg's command buffer through sg's Metal native scope, meeting sg's encoders at the
/// scope's fence; sr's guides reach it through one SGL pass that writes them in the forms it reads.
/// It runs where the build has the Metal backend and the device runs MetalFX's MTL4 scaler (`SR_HAS_METALFX`).
///
/// It upscales and does not denoise: a noisy input comes out noisy or smeared.
/// `sr::reconstruct_routine` runs it behind a denoise member; call it directly only on an image that is already clean.
class sr::metalfx_upscale_routine : public sg::render_routine<metalfx_upscale_routine>
{
public:
    /// Upscales `in.color` into `in.output`, carrying `history` from call to call.
    ///
    /// MetalFX writes only an output with both `image` and `render_target` usage; any other is written through an image
    /// of the routine's own and carried across after.
    /// An `image` output in `rgba16_float` or `rgba32_float` is copied by a compute pass, a `render_target` one is drawn
    /// in by `sr::blit_routine` in any renderable format, and one with neither is `failed`.
    /// `pending` while the passes compile, or the blit for a new output format does, and then nothing was recorded.
    /// `failed` when one does not build or MetalFX refuses the formats.
    /// `unsupported` off Metal, on a device without MetalFX, or at a ratio past the device's largest.
    [[nodiscard]] static upscale_outcome execute(sg::command_list& cmd, upscale_inputs const& in, upscale_history& history);

    /// Whether this build, `ctx` and its device can run it.
    [[nodiscard]] static bool is_available(sg::context const& ctx);

    /// The ratio of output to input `preset` runs at, per axis: 1, 1.5, 1.7 or 2, as FSR's.
    [[nodiscard]] static f32 ratio_of(render_scale_preset preset);

    /// Frame `frame_index`'s sub-pixel offset, in input pixels, as sr's jitter convention reads it.
    /// A Halton (2, 3) sequence whose period is 8 times the square of the ratio of the two extents, as FSR's.
    [[nodiscard]] static tg::vec2f jitter(u32 frame_index, tg::vec2i input_extent, tg::vec2i output_extent);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    impl::metalfx_passes _passes;
};
