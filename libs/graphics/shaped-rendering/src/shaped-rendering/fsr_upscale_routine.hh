#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/reconstruct.hh>

#include <memory>

/// Options only the FSR upscaler has.
struct sr::fsr_options
{
    /// FSR's own contrast-adaptive sharpening (RCAS) on the upscaled image.
    /// Separate from `reconstruct_settings::sharpness`, which steers the denoisers, so the two can be tuned apart.
    bool sharpening = false;

    /// In [0, 1]; only read with `sharpening` on.
    f32 sharpness = 0.5f;

    /// The time since the previous call, which FSR scales a few decay rates by.
    f32 frame_time_ms = 1000.0f / 60.0f;
};

/// AMD FSR 3.1's analytic upscaler, run through sg on any GPU.
///
/// AMD's own host code plans each frame — which of its passes run, with which constants, against which of its images —
/// and sr's backend executes that plan as ordinary sg dispatches, with sg inferring every barrier.
/// So it needs no vendor runtime and no particular vendor: it runs on hardware adapters on dx12 and vulkan, and not on
/// WARP yet, which crashes executing it.
/// It is available where the FidelityFX sources were fetched (`SR_HAS_FSR`), and needs HLSL through DXC today.
///
/// It upscales and does not denoise: a noisy input comes out noisy or smeared.
/// `sr::reconstruct_routine` runs it behind a denoise member; call it directly only on an image that is already clean.
class sr::fsr_upscale_routine : public sg::render_routine<fsr_upscale_routine>
{
public:
    /// Upscales `in.color` into `in.output`, carrying `history` from call to call.
    ///
    /// Any output extent at least the input's works; `render_scale_preset` names the ratios FSR is tuned for.
    /// `pending` while the passes compile, `failed` when one does not build, and `unsupported` without SR_HAS_FSR or
    /// on a software adapter.
    [[nodiscard]] static upscale_outcome execute(sg::command_list& cmd,
                                                 upscale_inputs const& in,
                                                 upscale_history& history,
                                                 fsr_options const& options = {});

    /// Whether this build, `ctx`'s shader library and its adapter can run it; never on a software adapter.
    [[nodiscard]] static bool is_available(sg::context const& ctx);

    /// The options the front's settings mean: `upscale_sharpness` and `frame_time_ms`.
    [[nodiscard]] static fsr_options options_for(reconstruct_settings const& settings);

    /// The ratio of output to input FSR runs `preset` at, per axis: 1, 1.5, 1.7 or 2.
    [[nodiscard]] static f32 ratio_of(render_scale_preset preset);

    /// Frame `frame_index`'s sub-pixel offset, in input pixels, as sr's jitter convention reads it.
    /// FSR's Halton (2, 3) sequence, whose period grows with the square of the ratio of the two extents.
    [[nodiscard]] static tg::vec2f jitter(u32 frame_index, tg::vec2i input_extent, tg::vec2i output_extent);

    /// Everything `init` built, shared with every stream's FSR context: the passes' pipelines and ours.
    /// Public for the backend in impl/, which is the only other reader.
    struct programs;

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    std::shared_ptr<programs const> _programs;
};
