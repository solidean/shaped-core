#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/buffer.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-rendering/fwd.hh>
#include <shaped-rendering/oidn_denoise_routine.hh>
#include <typed-geometry/linalg/vec.hh>

/// Open Image Denoise's trained U-Net, run as our own compute shaders.
///
/// The weights are Intel's and the inference is ours.
/// libs/graphics/shaped-rendering/docs/denoising.md says why, and what it measures.
///
/// The TOPOLOGY is fixed here and the WIDTHS come from the weights file, which is the split that keeps a weights bump
/// honest: a changed layer count fails to find its tensor, and a changed width fails the shape test beside it.
namespace sr::impl
{
/// fp16 bits widened to fp32, subnormals, infinities and NaNs included.
///
/// Local because typed-geometry has no half conversion yet; it is the one place the repo reads fp16.
[[nodiscard]] f32 half_to_float(u16 h);

/// The weights file, checked against the network's topology and packed for the convolution shader.
///
/// None of it depends on an image, so one copy serves every network in the process.
struct oidn_weights
{
    /// Every layer's weights as [ky][kx][i][o] over the padded channel counts, each followed by its bias.
    cc::vector<f32> packed;

    /// Where each convolution's weights and bias start in `packed`, in elements, in the order the layers run.
    cc::vector<u32> weight_offsets;
    cc::vector<u32> bias_offsets;

    /// How many channels and which resolution level each feature map holds, channel counts padded to four.
    cc::vector<i32> feature_channels;
    cc::vector<i32> feature_levels;

    /// Each convolution's padded input and output widths, which is what its dispatch is told.
    cc::vector<i32> in_channels;
    cc::vector<i32> out_channels;
};

/// The weights of one network, read and packed on the first call for it and shared by every later one.
///
/// Null when they were not fetched, or are not the topology this was written against; that is logged once.
[[nodiscard]] oidn_weights const* oidn_load_weights(oidn_network_size size);

/// Whether the trained weights were fetched into this build, which is what the member's availability rests on.
///
/// Answered once per process.
///
/// About the WEIGHTS rather than about OIDN the library: the member runs the network itself, so the library beside it
/// is a reference implementation for tests rather than something the render path needs.
[[nodiscard]] bool oidn_weights_present();

/// The five pipelines the network dispatches, with the binding layouts they are built against.
///
/// Separate from the network because they depend on nothing an image does: every stream, at every extent, dispatches
/// these same five.
/// That is what lets the member build them once in `init` rather than on a first frame — `ctx.cached` hands back the
/// same pipeline either way, so a network created later finds them already built.
struct oidn_programs
{
    sg::binding_group_layout_handle conv_layout = nullptr;
    sg::binding_group_layout_handle input_layout = nullptr;
    sg::binding_group_layout_handle output_layout = nullptr;
    sg::binding_group_layout_handle pool_layout = nullptr;
    sg::binding_group_layout_handle upsample_layout = nullptr;

    sg::async_compute_pipeline conv = nullptr;
    sg::async_compute_pipeline input = nullptr;
    sg::async_compute_pipeline output = nullptr;
    sg::async_compute_pipeline pool = nullptr;
    sg::async_compute_pipeline upsample = nullptr;

    /// Acquires the layouts, then builds whichever pipelines have finished compiling since the last call.
    /// Returns what `is_ready` would.
    bool build(sg::context& ctx);

    /// Whether all five have finished building, which is what a dispatch needs.
    [[nodiscard]] bool is_ready() const;
};

/// Drives `oidn_programs::build` to completion, so a network created afterwards is ready on its first `prepare`.
///
/// This is what the member's `init` awaits.
/// Building them lazily instead would leave a caller that advances an epoch every frame — which is every real frame
/// loop — waiting on a compile it never pumps.
/// False when a shader failed to compile, which is logged.
[[nodiscard]] cc::shared_async<bool> oidn_prewarm_pipelines(sg::context& ctx);

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

    /// The largest tile the network may run at, in pixels, before padding.
    ///
    /// A CAP rather than the size used: `create` picks the tile under it that computes the fewest pixels, which is
    /// usually smaller and never larger.
    /// It trades feature-map memory against the overlap computed twice, never against quality.
    /// libs/graphics/shaped-rendering/docs/denoising.md has the measured time and memory per cap.
    static constexpr int k_default_tile = 512;

    /// How much of a tile is discarded on each side, so its interior sees what a whole-image run would.
    ///
    /// 80 is where the network's receptive field is covered, and it was measured rather than derived.
    /// The same image tiled and whole agrees to a mean below 1e-6 at 80, is 1.4e-03 out at 64, and 2.8e-01 out with
    /// no overlap at all.
    /// So this is not a tolerance to trade against — below it the answer is wrong, and above it nothing improves.
    static constexpr int k_tile_overlap = 80;

    /// Takes the process's weights (read on the first create) and allocates every feature map for `image_extent`.
    ///
    /// The TENSORS are allocated at that size rounded up to a multiple of 16, because four pools halve it four times.
    /// The padding repeats the image's edge rather than being black, and nothing is written back for it.
    /// False when the weights are missing or are not the network this was written against, which is logged once.
    ///
    /// `max_tile` caps the tensors rather than the image: an image larger than it is run in overlapping tiles, and
    /// only a tile's interior reaches the output.
    /// An image that fits in one tile is run whole, with no overlap and no seams to answer for.
    ///
    /// `overlap` is how much of each tile is discarded per side; it is a parameter so a test can price it rather than
    /// trust it, and `k_tile_overlap` is the figure that pricing settled on.
    [[nodiscard]] bool create(sg::context& ctx,
                              tg::vec2i image_extent,
                              int max_tile = k_default_tile,
                              int overlap = k_tile_overlap,
                              oidn_network_size size = oidn_network_size::base);

    /// What `create` was asked for, which is what a caller compares against to know whether to create again.
    [[nodiscard]] oidn_network_size size() const { return _size; }
    [[nodiscard]] int max_tile() const { return _max_tile; }

    /// Builds whichever pipelines have finished compiling since the last call.
    ///
    /// Separate from `create` because the shaders compile in the background: a first call sees none of them, and the
    /// caller drives this until `is_ready` rather than blocking on a compile.
    /// Returns what `is_ready` would.
    bool prepare();

    /// Whether every pipeline has finished building, which is what `execute` needs.
    [[nodiscard]] bool is_ready() const;

    [[nodiscard]] bool is_valid() const { return _weights.raw() != nullptr; }
    /// The image this was created for, which is what `execute` reads and writes.
    [[nodiscard]] tg::vec2i extent() const { return _image_extent; }

    /// The tensors' extent, which is one tile rounded up to a multiple of 16 — the whole image when it fits in one.
    [[nodiscard]] tg::vec2i padded_extent() const { return _extent; }

    /// How many tiles one `execute` runs, which is 1 whenever the image fits a tile.
    [[nodiscard]] tg::vec2i tile_counts() const { return _tile_counts; }

    /// How much of a tile is kept, and how much of it is the overlap discarded on each side.
    [[nodiscard]] tg::vec2i tile_step() const { return _tile_step; }
    [[nodiscard]] int tile_overlap() const { return _overlap; }

    /// What the feature maps cost at `image_extent`, in bytes, without allocating anything.
    ///
    /// Answered from the layer widths this network already read, so it needs a created network but not one at that
    /// size — which is the only way to ask the question for an extent too large to allocate.
    /// The weights themselves are not counted: they are a fixed few megabytes, and one host copy serves every network.
    [[nodiscard]] i64 feature_bytes_for(tg::vec2i image_extent) const;

    /// What this network ACTUALLY allocated, which is one tile's worth however large the image is.
    [[nodiscard]] i64 feature_bytes() const;

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
    /// Creates the tile-invariant binding groups, once the pipelines exist to say what they bind against.
    void build_groups();

    sg::context* _ctx = nullptr;
    oidn_network_size _size = oidn_network_size::base;
    int _max_tile = 0;
    tg::vec2i _image_extent = tg::vec2i(0, 0);
    tg::vec2i _extent = tg::vec2i(0, 0); // the padded one the tensors are sized by — one TILE, not the image

    /// How far the interior of one tile advances, and how wide the discarded border around it is.
    /// `_overlap` is 0 exactly when the image fits one tile, so a small image is run whole and has no seams at all.
    /// An axis that fits under the cap has a count of 1 and a step of its whole extent even when the other axis tiles.
    tg::vec2i _tile_step = tg::vec2i(0, 0);
    tg::vec2i _tile_counts = tg::vec2i(1, 1);
    int _overlap = 0;

    /// The weights and every shape derived from them, read once per process and shared by every network.
    oidn_weights const* _source = nullptr;

    /// Every layer's weights and bias, in one buffer; a layer is a pair of offsets into it.
    sg::buffer<f32> _weights;

    /// Whether `_weights` has been filled yet.
    ///
    /// Filled on the caller's command list by the first `execute`, never by `create`: a create that submitted would
    /// put work on the queue outside any frame, and a caller draining its own epoch would never see it.
    bool _weights_uploaded = false;

    /// The feature maps, indexed by the table in the implementation.
    /// One per tensor rather than a ping-pong: the skips have to stay live across the decoder, and an aliasing
    /// mistake here is a wrong image rather than a crash.
    cc::vector<sg::buffer<f32>> _features;

    oidn_programs _programs;

    /// The binding groups the network dispatches against, built once and reused by every tile and every frame.
    ///
    /// They are tile-invariant on purpose: a tile changes the push constants and nothing a group names.
    /// Building them per tile would mean 26 groups per tile, 624 over the 24 tiles of a 1080p frame at the default cap.
    /// The input and output groups are NOT here, because they name the caller's textures rather than ours.
    cc::vector<sg::binding_group_handle> _conv_groups;
    cc::vector<sg::binding_group_handle> _pool_groups;
    cc::vector<sg::binding_group_handle> _upsample_groups;
};
} // namespace sr::impl
