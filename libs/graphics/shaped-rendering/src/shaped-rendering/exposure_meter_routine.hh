#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// Meters an image for an automatic exposure: the mean log2 luminance over a 64x64 grid of its cells, each the linear mean of the pixels spread over it.
/// The log of a cell rather than of a pixel, so an image of few samples meters as its converged self would rather than darker.
///
/// The GPU writes 256 partial sums and the caller finishes the mean on the host, after reading them back.
/// So a meter is a frame or two behind the image it measured, which an exposure that adapts smoothly never shows.
/// `average_luminance` turns the partials into the luminance, and `sr::exposure_for_average` into an exposure.
class sr::exposure_meter_routine : public sg::render_routine<exposure_meter_routine>
{
public:
    static constexpr int partial_count = 256;

    /// The luminance a black cell meters at, `darkest` in exposure_meter.sgl, which must match.
    /// An image that meters at it holds nothing to expose for, like the black frames while a renderer's shaders compile.
    static constexpr f32 darkest = 0.0001f;

    /// Meters `image`, which must be a float or normalized color texture with `texture` usage, into a fresh buffer of `partial_count` partials.
    /// The buffer is the frame's, with `copy_src` usage, for the caller to read back: `cmd.download.data_from_buffer(partials)`.
    /// None while the shader is still compiling, or after a compile that did not build, which init logged.
    [[nodiscard]] static cc::optional<sg::buffer<tg::vec2f>> execute(sg::command_list& cmd, sg::texture_2d const& image);

    /// The luminance the partials meter: 2 to the mean of their log2 luminance, or none where they sum no samples.
    [[nodiscard]] static cc::optional<f32> average_luminance(cc::span<tg::vec2f const> partials);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;

private:
    sg::binding_group_layout_handle _group_layout;
    sg::async_compute_pipeline _pipeline;
};
