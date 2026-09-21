#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/denoise.hh>
#include <shaped-rendering/fwd.hh>

/// Options only the OIDN member has.
struct sr::oidn_options
{
    /// What the radiance is multiplied by before the network sees it, and divided by afterwards.
    ///
    /// The network was trained over a particular range of brightness, and this is what brings a scene into it.
    /// Unlike NRD — which forbids a premultiplied exposure — OIDN asks for one: left to itself it MEASURES the image
    /// and derives its own, and this is `options_for` handing it the caller's instead.
    f32 input_scale = 1.0f;
};

/// Intel Open Image Denoise: a trained spatial denoiser, run as our own compute shaders.
///
/// **The weights are Intel's and the inference is ours.**
/// OIDN's own GPU kernels are CUDA, HIP, SYCL and Metal sources built on vendor GEMM libraries, and its CPU device
/// would mean a download and an upload every frame — so shaped-rendering runs the network itself, which is what makes
/// this the one trained member that needs no vendor's hardware and no memory shared across two APIs.
///
/// That it computes what Intel computes is measured rather than assumed: `oidn-network-test.cc` compares a denoised
/// image against OIDN's own filter, and the docs carry the number.
///
/// **Only where the weights were fetched** — `extern/oidn-weights`, which dev.py hydrates on demand.
/// Everywhere else this routine still exists and reports `unsupported`.
///
/// **Spatial, so it denoises a converging mean** rather than one frame's samples, and it reads no history.
/// It requires an albedo and a normal, because the network it runs has nine input channels and those are six of them.
class sr::oidn_denoise_routine : public sg::render_routine<oidn_denoise_routine>
{
public:
    /// Denoises `in.color` into `in.output`, carrying the network in `history`.
    ///
    /// Does not upscale: `in.output` must be the input's extent.
    /// The image may be any size; the network pads its own tensors up to what four pools need.
    [[nodiscard]] static denoise_outcome execute(sg::command_list& cmd,
                                                 denoise_inputs const& in,
                                                 denoise_history& history,
                                                 oidn_options const& options = {});

    /// What the shared knobs map onto.
    ///
    /// `exposure` becomes the input scale, which is the one shared knob this member genuinely wants.
    /// `quality` maps onto nothing: OIDN publishes small, base and large networks, and only the base one is fetched —
    /// see extern/oidn-weights/dependency.yml for why.
    [[nodiscard]] static oidn_options options_for(denoise_settings const& settings);

    /// Whether this build can run it, which is whether the weights were fetched.
    /// It asks nothing of the device: the network is ordinary compute.
    [[nodiscard]] static bool is_available(sg::context const& ctx);

protected:
    /// Compiles the five shaders the network is made of, so a first call finds them built.
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;
};
