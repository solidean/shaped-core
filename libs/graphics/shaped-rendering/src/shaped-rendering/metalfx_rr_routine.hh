#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/impl/metalfx_passes.hh>
#include <shaped-rendering/reconstruct.hh>

/// Options only the MetalFX denoised scaler has.
struct sr::metalfx_rr_options
{
    /// The multiplier the caller will apply to the image before display, which MetalFX reads from a texel of its own.
    f32 exposure = 1.0f;
};

/// Apple MetalFX's temporal denoised scaler (`MTL4FXTemporalDenoisedScaler`): a temporal denoiser that upscales as part
/// of denoising, as `sr::dlss_rr_routine` is on NVIDIA.
///
/// It runs where the build has the Metal backend and the device runs MetalFX's MTL4 denoised scaler; everywhere else
/// it reports `unsupported`.
/// It requires every guide `sr::required_guides(denoise_method::metalfx_rr)` names, and reads `world_to_view` and
/// `view_to_clip` from the guides beside them.
///
/// **Temporal, so it wants this frame's own samples**, with noise that is uncorrelated between pixels, rather than a
/// converging mean.
/// Its scaler lives in the caller's `sr::reconstruct_history`; a new extent or a new format of any texture builds a
/// new one, and a `reset` restarts the one it has.
class sr::metalfx_rr_routine : public sg::render_routine<metalfx_rr_routine>
{
public:
    /// Denoises `in.color` into `in.output`, carrying `history` from call to call.
    ///
    /// `in.output` may be larger than `in.color`, by a ratio `sr::reconstruct_input_extent` produced.
    /// MetalFX writes only an output with both `image` and `render_target` usage; any other is written through an image
    /// of the routine's own and carried across after.
    /// An `image` output in `rgba16_float` or `rgba32_float` is copied by a compute pass, a `render_target` one is drawn
    /// in by `sr::blit_routine` in any renderable format, and one with neither is `failed`.
    [[nodiscard]] static reconstruct_outcome execute(sg::command_list& cmd,
                                                     reconstruct_inputs const& in,
                                                     reconstruct_history& history,
                                                     metalfx_rr_options const& options = {});

    /// What the shared knobs map onto: `exposure` alone.
    [[nodiscard]] static metalfx_rr_options options_for(reconstruct_settings const& settings);

    /// Whether this build, `ctx` and its device can run it.
    [[nodiscard]] static bool is_available(sg::context const& ctx);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    impl::metalfx_passes _passes;
};
