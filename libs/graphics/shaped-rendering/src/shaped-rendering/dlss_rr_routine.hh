#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/reconstruct.hh>

#include <memory> // std::shared_ptr, which is how the routine shares its NGX instance with every stream

namespace sr::impl
{
struct dlss_instance;
}

/// Options only the DLSS Ray Reconstruction member has.
struct sr::dlss_options
{
    /// Which of NGX's presets the feature is created with, which selects the network as well as its cost.
    /// A creation parameter: changing it between calls restarts the stream.
    sr::denoise_quality quality = sr::denoise_quality::balanced;

    /// Whether the radiance handed over is linear HDR, which everything sv traces is.
    /// A caller passing tone-mapped colour clears it, and NGX judges brightness differently.
    /// A creation parameter like `quality`, so changing it restarts the stream.
    bool hdr = true;

    /// The multiplier the caller will apply to the image before display.
    /// NGX judges noise by how bright a pixel ends up on screen, so a scene denoised at 4 and shown at 4 is not the
    /// same picture as one denoised at 1 — this is what keeps the two agreeing.
    f32 exposure = 1.0f;
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
/// **Temporal, so it wants this frame's own samples** rather than a converging mean — see `sr::reconstruct_routine`, which
/// picks between the two.
/// Its per-stream feature lives in the caller's `sr::reconstruct_history`.
/// A new extent, `quality` or `hdr` builds a new one, and a `reset` restarts the one it has.
class sr::dlss_rr_routine : public sg::render_routine<dlss_rr_routine>
{
public:
    /// Denoises `in.color` into `in.output`, carrying `history` from call to call.
    ///
    /// `in.output` may be larger than `in.color` — this is the first member that upscales — and any ratio between them
    /// must be one `sr::reconstruct_input_extent` produced.
    [[nodiscard]] static reconstruct_outcome execute(sg::command_list& cmd,
                                                     reconstruct_inputs const& in,
                                                     reconstruct_history& history,
                                                     dlss_options const& options = {});

    /// What the shared knobs map onto: `quality` picks the NGX preset, `exposure` becomes NGX's pre-exposure.
    /// `exposure` is read per call; `quality` and `hdr` are creation parameters, and changing one restarts the stream.
    [[nodiscard]] static dlss_options options_for(reconstruct_settings const& settings);

    /// Whether this build and this device can run it, which is what `sr::query_reconstruct_support` reports.
    [[nodiscard]] static bool is_available(sg::context const& ctx);

    /// Closes NGX for the device once the GPU is done with it, through `ctx.defer_until_retired`.
    ///
    /// Never directly: a context's shutdown clears its routines before its final drain, so NGX work may still be
    /// executing when this runs.
    ~dlss_rr_routine() override;

protected:
    /// Opens NGX for the context's device; nothing is compiled, since the networks are the runtime's.
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    /// Null until init, and where the device cannot run DLSS.
    std::shared_ptr<impl::dlss_instance> _instance;
    sg::context* _ctx = nullptr;
};
