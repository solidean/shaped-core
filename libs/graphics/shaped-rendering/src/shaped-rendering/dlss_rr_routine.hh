#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/denoise.hh>
#include <shaped-rendering/fwd.hh>

/// Options only the DLSS Ray Reconstruction member has.
struct sr::dlss_options
{
    /// Which of NGX's presets the feature is created with, which selects the network as well as its cost.
    /// 0 fastest, 1 balanced, 2 best — `options_for` maps `denoise_quality` onto it.
    int quality = 1;

    /// Whether the radiance handed over is linear HDR, which everything sv traces is.
    /// A caller passing tone-mapped colour clears it, and NGX judges brightness differently.
    bool hdr = true;
};

/// NVIDIA DLSS Ray Reconstruction: a temporal denoiser that upscales as part of denoising.
///
/// **Only where the SDK was fetched** — `extern/dlss/fetch-dlss.py`, which nothing runs for you — and only on dx12,
/// on an adapter and driver that carry the feature.
/// Everywhere else this routine still exists and reports `unsupported`, so a caller naming it compiles everywhere and
/// is told plainly where it cannot run.
///
/// It requires every guide `sr::required_guides(denoise_method::dlss_rr)` names, the specular albedo and roughness
/// included; a call missing one reports `unsupported` rather than running degraded.
///
/// **Temporal, so it wants this frame's own samples** rather than a converging mean — see `sr::denoise_routine`, which
/// picks between the two.
/// Its per-stream feature lives in the caller's `sr::denoise_history`, and a new extent or a `reset` builds a new one.
class sr::dlss_rr_routine : public sg::render_routine<dlss_rr_routine>
{
public:
    /// Denoises `in.color` into `in.output`, carrying `history` from call to call.
    ///
    /// `in.output` may be larger than `in.color` — this is the first member that upscales — and any ratio between them
    /// must be one `sr::denoise_input_extent` produced.
    [[nodiscard]] static denoise_outcome execute(sg::command_list& cmd,
                                                 denoise_inputs const& in,
                                                 denoise_history& history,
                                                 dlss_options const& options = {});

    /// What the shared knobs map onto: `quality` picks the NGX preset, `exposure` rides on the call.
    [[nodiscard]] static dlss_options options_for(denoise_settings const& settings);

    /// Whether this build and this device can run it, which is what `sr::query_denoise_support` reports.
    [[nodiscard]] static bool is_available(sg::context const& ctx);

protected:
    /// Nothing to compile: the networks are the runtime's, and the feature is per stream rather than per context.
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;
};
