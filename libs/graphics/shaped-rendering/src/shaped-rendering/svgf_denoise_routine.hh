#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/denoise.hh>
#include <shaped-rendering/fwd.hh>

/// Options only the SVGF member has.
struct sr::svgf_options
{
    /// Wavelet passes after the temporal and variance passes, each doubling the tap spacing.
    int iterations = 4;

    /// How many measured noise widths a luminance difference may span and still average.
    f32 luminance_sigma = 4.0f;

    /// How closely normals must agree, as the exponent on their dot product.
    f32 normal_power = 128.0f;

    /// How closely depths must agree, as a fraction of the centre's depth per pixel of tap distance.
    f32 depth_sigma = 0.02f;

    /// The smallest weight a new frame gets in the colour history once it is long; higher follows change faster and is noisier.
    f32 color_alpha_min = 0.2f;

    /// The same for the luminance moments the variance comes from.
    f32 moments_alpha_min = 0.2f;

    /// Where the history length stops growing.
    f32 max_history = 32.0f;

    /// A history shorter than this estimates its variance from its neighbours rather than from itself.
    f32 spatial_variance_below = 4.0f;

    /// The least cosine between this frame's normal and the reprojected one that still counts as the same surface.
    f32 normal_similarity = 0.9f;

    /// The largest relative depth change between this frame and the reprojected one that still counts as the same surface.
    f32 depth_similarity = 0.1f;
};

/// Spatiotemporal variance-guided filtering (Schied et al. 2017): à-trous steered by a noise estimate that history measures.
///
/// Temporal and native.
/// It needs fresh samples every call — this frame's own, not a running mean — plus normal, depth and motion guides, and
/// a history carried from the call before in the caller's `denoise_history`.
/// Albedo is optional, and divided out before anything accumulates, so texture detail survives both halves.
///
/// Three kinds of pass, one dispatch each:
/// - **temporal**: follows each pixel's motion back, keeps the history there if the surface matches, and blends in this
///   frame's colour and luminance moments;
/// - **variance**: turns the moments into a per-pixel noise estimate, spatially while the history is too short to know;
/// - **à-trous**, `iterations` times: the edge-avoiding wavelet filter, its luminance edge-stop that estimate.
///
/// The colour history it carries is the integrated, unfiltered one, which is simpler than feeding a filtered pass back
/// and keeps the filter from compounding across frames.
/// A `restarted` call, or a `denoise_history::reset`, starts every pixel from this frame's samples alone.
class sr::svgf_denoise_routine : public sg::render_routine<svgf_denoise_routine>
{
public:
    /// Filters `in.color`, a fresh single-frame estimate, into `in.output`, carrying `history` to the next call.
    ///
    /// `in.output` must match `in.color`'s extent and carry `readwrite_texture` usage; SVGF does not upscale.
    /// The normal, depth and motion guides are required, and asserted on.
    /// `pending` while the shaders compile, `failed` after a compile that did not build.
    [[nodiscard]] static denoise_outcome execute(sg::command_list& cmd,
                                                 denoise_inputs const& in,
                                                 denoise_history& history,
                                                 svgf_options const& options = {});

    /// What the shared knobs map onto: `quality` picks the pass count, `sharpness` the luminance sigma, and
    /// `temporal_responsiveness` the history's floor weight.
    [[nodiscard]] static svgf_options options_for(denoise_settings const& settings);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    struct pass
    {
        sg::binding_group_layout_handle group_layout;
        sg::compute_pipeline_handle pipeline;
    };

    pass _temporal;
    pass _variance;
    pass _atrous;
};
