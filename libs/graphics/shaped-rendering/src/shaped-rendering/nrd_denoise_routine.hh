#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/denoise.hh>
#include <shaped-rendering/fwd.hh>

/// Options only the NRD member has.
///
/// No exposure among them, unlike `dlss_rr`: NRD's own input contract says radiance must NOT be premultiplied by one,
/// so a knob here would be a promise to do something the library forbids.
struct sr::nrd_options
{
    /// How long REBLUR's two histories may grow, in frames.
    /// Longer is smoother and slower to react; `options_for` maps `denoise_quality` onto the pair, and these are NRD's
    /// own defaults.
    u32 max_accumulated_frames = 30;
    u32 max_fast_accumulated_frames = 6;
};

/// NVIDIA Real-Time Denoisers (REBLUR): a temporal denoiser for a split diffuse and specular signal.
///
/// **Only where the SDK sources were fetched** — `extern/nrd/fetch-nrd.py`, which nothing runs for you.
/// Everywhere else this routine still exists and reports `unsupported`.
///
/// Unlike DLSS it has no device requirement at all.
/// NRD compiles nothing at run time and records nothing: it answers which compute dispatches would denoise this frame,
/// and shaped-rendering runs them through sg — so this is the one vendor member that works on any adapter, WARP
/// included, and the only split-signal member that can be tested without the hardware that shipped it.
///
/// It requires every guide `sr::required_guides(denoise_method::nrd)` names, `split_diffuse_specular` among them: the
/// denoiser's whole premise is that the two lobes blur differently, so a call carrying one radiance texture reports
/// `unsupported` rather than denoising the sum as if it were diffuse.
///
/// **Temporal, so it wants this frame's own samples** rather than a converging mean.
/// Its NRD instance is the history, and lives in the caller's `sr::denoise_history`.
class sr::nrd_denoise_routine : public sg::render_routine<nrd_denoise_routine>
{
public:
    /// Denoises `in.color` (diffuse) and `in.specular` into `in.output`, carrying `history` from call to call.
    ///
    /// Does not upscale: REBLUR is a denoiser, so `in.output` must be the input's extent.
    [[nodiscard]] static denoise_outcome execute(sg::command_list& cmd,
                                                 denoise_inputs const& in,
                                                 denoise_history& history,
                                                 nrd_options const& options = {});

    [[nodiscard]] static nrd_options options_for(denoise_settings const& settings);

    /// Whether this build can run it, which is what `sr::query_denoise_support` reports.
    /// It asks nothing of the device, so this is really "were the sources fetched".
    [[nodiscard]] static bool is_available(sg::context const& ctx);

protected:
    /// Builds the two passes that bracket NRD: the repack into its encodings, and the decode back out of them.
    /// NRD's own pipelines are per stream rather than per context, and build inside the session.
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::binding_group_layout_handle _repack_layout = nullptr;
    sg::binding_group_layout_handle _resolve_layout = nullptr;
    sg::compute_pipeline_handle _repack_pipeline = nullptr;
    sg::compute_pipeline_handle _resolve_pipeline = nullptr;
};
