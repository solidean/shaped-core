#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/denoise.hh>
#include <shaped-rendering/fwd.hh>

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
/// Spatial and native: HLSL through DXC, and the member CI can test.
/// It requires no guide, and uses whichever of albedo, normal and depth it is given.
///
/// **Its luminance edge-stop scales with `1 / sqrt(sample_count)`.**
/// The noise in a Monte Carlo mean shrinks that way as it converges, so a one-sample frame is filtered hard, while an
/// image a thousand samples deep is averaged only across differences a few of its (by then tiny) noise widths wide.
/// The model is one noise width per sample equal to the pixel's own luminance, which is crude and deliberately
/// conservative: a real integrator's per-sample noise is usually larger, so real detail survives with margin.
/// That is what lets it sit on an accumulating mean without blurring the converged result.
///
/// Holds its two ping-pong images in the caller's `denoise_history`, so a steady stream allocates nothing.
/// It keeps no history in the temporal sense, so a `restarted` call looks exactly like any other.
class sr::atrous_denoise_routine : public sg::render_routine<atrous_denoise_routine>
{
public:
    /// Filters `in.color` into `in.output`.
    ///
    /// `in.output` must match `in.color`'s extent — à-trous does not upscale — and carry `readwrite_texture` usage.
    /// `pending` while the shader compiles, `failed` after a compile that did not build.
    [[nodiscard]] static denoise_outcome execute(sg::command_list& cmd,
                                                 denoise_inputs const& in,
                                                 denoise_history& history,
                                                 atrous_options const& options = {});

    /// What the shared knobs map onto: `quality` picks the pass count, `sharpness` the luminance sigma.
    [[nodiscard]] static atrous_options options_for(denoise_settings const& settings);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::binding_group_layout_handle _group_layout;
    sg::compute_pipeline_handle _pipeline;
};
