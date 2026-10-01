#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/reconstruct.hh>

/// Which of OIDN's trained networks runs: the same topology, at two widths.
enum class sr::oidn_network_size : sg::u8
{
    /// Half the compute of `base` at the same receptive field, and visibly softer; OIDN's own `fast` quality.
    small,
    /// OIDN's `balanced` quality, and for HDR with an albedo and a normal also its `high`, since no large one exists.
    base,
};

/// Options only the OIDN member has.
struct sr::oidn_options
{
    /// What the radiance is multiplied by before the network sees it, and divided by afterwards.
    ///
    /// The network was trained over a particular range of brightness, and this is what brings a scene into it.
    /// Unlike NRD — which forbids a premultiplied exposure — OIDN asks for one: left to itself it MEASURES the image
    /// and derives its own, and this is `options_for` handing it the caller's instead.
    f32 input_scale = 1.0f;

    /// The largest tile the network may run at, which trades memory against the overlap computed twice.
    ///
    /// A cap rather than the size used, and 0 takes the network's own default of 512.
    /// It never changes the image.
    /// libs/graphics/shaped-rendering/docs/reconstruction.md has the measured time and memory per cap.
    i32 max_tile = 0;

    /// Which trained network runs.
    oidn_network_size network = oidn_network_size::base;
};

/// Intel Open Image Denoise: a trained spatial denoiser, run as our own compute shaders.
///
/// **The weights are Intel's and the inference is ours**, so it needs no vendor SDK and no particular GPU.
/// Its output is held to Intel's own filter by a test.
/// libs/graphics/shaped-rendering/docs/reconstruction.md has why it runs this way, and what it costs.
///
/// **Far too slow for a frame loop** — roughly 0.2 s per megapixel — so `automatic` never picks it; name it.
///
/// **Only where the weights were fetched** (`extern/oidn-weights`, a default fetch) and on a backend its HLSL
/// compiles for; everywhere else this routine still exists and reports `unsupported`.
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
    [[nodiscard]] static reconstruct_outcome execute(sg::command_list& cmd,
                                                     reconstruct_inputs const& in,
                                                     reconstruct_history& history,
                                                     oidn_options const& options = {});

    /// What the shared knobs map onto.
    ///
    /// `exposure` becomes the input scale, which is the one shared knob this member genuinely wants.
    /// `quality` picks the network the way OIDN's own setting does: `fast` runs the small one, the rest the base one.
    [[nodiscard]] static oidn_options options_for(reconstruct_settings const& settings);

    /// Whether this build and context can run it: the weights were fetched, and its shaders build here.
    [[nodiscard]] static bool is_available(sg::context const& ctx);

protected:
    /// Compiles the five shaders the network is made of, so a first call finds them built.
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;
};
