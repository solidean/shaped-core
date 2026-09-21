#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// Open Image Denoise's trained U-Net, run as our own compute shaders.
///
/// The weights are Intel's and the inference is ours, which is the whole point: OIDN's own GPU kernels are CUDA, HIP,
/// SYCL and Metal built on vendor GEMM libraries, and its CPU device would mean a download and an upload every frame.
/// The network is sixteen 3x3 convolutions, four max pools and four nearest upsamples, so it runs wherever sg does.
///
/// The TOPOLOGY is fixed here and the WIDTHS come from the weights file, which is the split that keeps a weights bump
/// honest: a changed layer count fails to find its tensor, and a changed width fails the shape test beside it.
namespace sr::impl
{
/// One image stream's network: the weights on the device, and the feature maps they are run through.
///
/// Move-only, and sized for one extent — the feature maps are a function of it, and there are twenty-five of them.
class oidn_network
{
public:
    oidn_network() = default;
    oidn_network(oidn_network&&) noexcept = default;
    oidn_network& operator=(oidn_network&&) noexcept = default;
    oidn_network(oidn_network const&) = delete;
    oidn_network& operator=(oidn_network const&) = delete;

    /// Reads the weights, uploads them, and allocates every feature map for `extent`.
    ///
    /// `extent` must be a multiple of 16 on both axes, because four pools halve it four times — a caller pads up to
    /// that and crops afterwards.
    /// False when the weights are missing or are not the network this was written against, which is logged once.
    [[nodiscard]] bool create(sg::context& ctx, tg::vec2i extent);

    /// Builds whichever pipelines have finished compiling since the last call.
    ///
    /// Separate from `create` because the shaders compile in the background: a first call sees none of them, and the
    /// caller drives this until `is_ready` rather than blocking on a compile.
    /// Returns what `is_ready` would.
    bool prepare();

    /// Whether every pipeline has finished building, which is what `execute` needs.
    [[nodiscard]] bool is_ready() const;

    [[nodiscard]] bool is_valid() const { return _weights.raw() != nullptr; }
    [[nodiscard]] tg::vec2i extent() const { return _extent; }

    /// Records the whole network onto `cmd`, from three guides to one denoised image.
    ///
    /// `input_scale` multiplies the radiance before the transfer curve and divides it back out afterwards, which is
    /// how a scene is brought into the range the network was trained over.
    /// False when a pipeline is still building.
    [[nodiscard]] bool execute(sg::command_list& cmd,
                               sg::texture_2d const& color,
                               sg::texture_2d const& albedo,
                               sg::texture_2d const& normal,
                               sg::texture_2d const& output,
                               f32 input_scale);

private:
    sg::context* _ctx = nullptr;
    tg::vec2i _extent = tg::vec2i(0, 0);

    /// Every layer's weights and bias, in one buffer; a layer is a pair of offsets into it.
    sg::buffer<f32> _weights;

    /// The same, host-side, until the first `execute` records its upload.
    ///
    /// Uploaded on the caller's command list rather than one of `create`'s own: a create that submitted would put
    /// work on the queue outside any frame, and a caller draining its own epoch would never see it.
    cc::vector<f32> _pending_weights;

    /// Where each convolution's weights and bias start, in elements, in the order the layers run.
    cc::vector<u32> _weight_offsets;
    cc::vector<u32> _bias_offsets;

    /// The feature maps, indexed by the table in the implementation.
    /// One per tensor rather than a ping-pong: the skips have to stay live across the decoder, and an aliasing
    /// mistake here is a wrong image rather than a crash.
    cc::vector<sg::buffer<f32>> _features;

    /// How many channels and which resolution level each feature map holds.
    /// Recorded rather than recovered from a buffer's size, because a buffer does not carry its shape and the two
    /// would have to agree anyway.
    cc::vector<i32> _feature_channels;
    cc::vector<i32> _feature_levels;

    /// Each convolution's input and output widths, read from the weights at creation.
    cc::vector<i32> _in_channels;
    cc::vector<i32> _out_channels;

    sg::binding_group_layout_handle _conv_layout = nullptr;
    sg::binding_group_layout_handle _input_layout = nullptr;
    sg::binding_group_layout_handle _output_layout = nullptr;
    sg::binding_group_layout_handle _pool_layout = nullptr;
    sg::binding_group_layout_handle _upsample_layout = nullptr;

    sg::async_compute_pipeline _conv = nullptr;
    sg::async_compute_pipeline _input = nullptr;
    sg::async_compute_pipeline _output = nullptr;
    sg::async_compute_pipeline _pool = nullptr;
    sg::async_compute_pipeline _upsample = nullptr;
};
} // namespace sr::impl
