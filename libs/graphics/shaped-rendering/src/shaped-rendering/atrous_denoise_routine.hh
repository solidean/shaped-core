#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/pixel_format.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/reconstruct.hh>

/// Options only the à-trous member has.
struct sr::atrous_options
{
    /// Wavelet passes, each doubling the tap spacing; 5 reaches 32 pixels out from the centre.
    int iterations = 4;

    /// How far a neighbour's luminance may differ from the centre's before it stops counting, in multiples of the
    /// noise expected at the input's sample count; the weight falls off as a Gaussian in that ratio.
    /// Higher removes more noise and more detail.
    f32 luminance_sigma = 2.25f;

    /// How closely normals must agree, as the exponent on their dot product.
    f32 normal_power = 64.0f;

    /// How closely depths must agree, as a fraction of the centre's depth per pixel of tap distance.
    f32 depth_sigma = 0.02f;

    /// Divide the albedo out before filtering and multiply it back after, so texture detail survives.
    /// Only takes effect with an albedo guide.
    bool demodulate_albedo = true;
};

/// Edge-avoiding à-trous wavelet denoising (Dammertz et al. 2010), steered by normal, depth and luminance.
///
/// Spatial and native: its shader is SGL, and it is the member CI can test.
/// It requires no guide, and uses whichever of albedo, normal and depth it is given.
///
/// **Its luminance edge-stop scales with `1 / sqrt(sample_count)`.**
/// The noise in a Monte Carlo mean shrinks that way as it converges, so a one-sample frame is filtered hard, while an
/// image a thousand samples deep is averaged only across differences a few of its (by then tiny) noise widths wide.
/// The model is one noise width per sample equal to the pixel's own luminance, which is crude and deliberately
/// conservative: a real integrator's per-sample noise is usually larger, so real detail survives with margin.
/// That is what lets it sit on an accumulating mean without blurring the converged result.
/// Given `reconstruct_inputs::sample_counts`, it takes the count per pixel instead, for a mean whose pixels differ in depth.
///
/// Holds its two ping-pong images in the caller's `reconstruct_history`, so a steady stream allocates nothing.
/// It keeps no history in the temporal sense, so a `restarted` call looks exactly like any other.
///
/// A render routine **parametrized on the output's format**, which its last pass writes and its shader names.
/// `prewarm(ctx, format)` starts the compile for the outputs a caller will hand it; `reconstruct_routine` prewarms it.
class sr::atrous_denoise_routine : public sg::render_routine<atrous_denoise_routine, sg::pixel_format>
{
public:
    /// Filters `in.color` into `in.output`.
    ///
    /// `in.output` must match `in.color`'s extent — à-trous does not upscale.
    /// It carries `image` usage, in a four-channel float or normalized format the device writes as an image.
    /// `pending` while the shader for that format compiles, `failed` after a compile that did not build.
    [[nodiscard]] static reconstruct_outcome execute(sg::command_list& cmd,
                                                     reconstruct_inputs const& in,
                                                     reconstruct_history& history,
                                                     atrous_options const& options = {});

    /// What the shared knobs map onto: `quality` picks the pass count, `sharpness` the luminance sigma.
    [[nodiscard]] static atrous_options options_for(reconstruct_settings const& settings);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    /// One compile of the pass: its group's layout follows the format its target names.
    struct pass
    {
        sg::binding_group_layout_handle group_layout;
        sg::async_compute_pipeline pipeline;
    };

    [[nodiscard]] static pass build_pass(sg::context& ctx, sg::pixel_format target);

    /// What every pass but the last writes, into the history's scratch images.
    pass _scratch;
    /// What the last pass writes, into the caller's output.
    pass _output;
};
