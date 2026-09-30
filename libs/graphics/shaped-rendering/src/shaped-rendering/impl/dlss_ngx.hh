#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-rendering/denoise.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

#include <memory> // std::shared_ptr, which is how a stream shares its open instance

/// The NGX seam: everything `sr::dlss_rr_routine` needs from NVIDIA's SDK, with none of its headers.
///
/// Two implementations, chosen by the build: `dlss_ngx.cc` where the SDK was fetched and dx12 exists, `dlss_null.cc`
/// otherwise.
/// That is what lets the routine and its whole public surface exist unconditionally — a caller naming `dlss_rr` always
/// compiles, and only the answer changes.
///
/// Everything here is dx12-only, because a vendor member needs `sg::backend::dx12::dx12_native_scope` to hand foreign
/// code a command list and no other backend has one.
///
/// **NGX is not thread-safe**, so every call into it, from every function below, is serialized by one process-wide lock.
namespace sr::impl
{
/// NGX opened for one device, by `dlss_open`; defined by the implementation.
///
/// Shared by the routine that opened it and every stream created under it, so a stream released after the routine's
/// close still has a record to find the close on.
struct dlss_instance;

/// What one Ray Reconstruction feature is created for.
///
/// All of it is fixed at creation: NGX builds its networks for a size, a preset and a colour space.
/// So a change to any field is a new feature rather than a parameter, which is why `denoise_history` drops one when an
/// extent moves and `dlss_rr_routine` does when `quality` or `hdr` does.
struct dlss_feature_desc
{
    tg::vec2i input_extent = tg::vec2i(0, 0);
    tg::vec2i output_extent = tg::vec2i(0, 0);
    denoise_quality quality = denoise_quality::balanced;

    /// Whether the radiance handed over is linear HDR, which ours is.
    bool hdr = true;
};

/// One stream's feature, its own parameter map, and what it was created with.
///
/// `feature` and `params` are NGX objects kept opaque, so this header stays NVIDIA-free.
/// The map is per stream because evaluate writes its per-frame values into it, and two streams sharing one would
/// race even under the lock, one stream's values surviving into the other's call.
struct dlss_stream
{
    void* feature = nullptr;
    void* params = nullptr;
    std::shared_ptr<dlss_instance> instance;
    denoise_quality quality = denoise_quality::balanced;
    bool hdr = true;
};

/// One evaluation's resources and per-frame values.
///
/// Every texture is at the input extent except `output`, which is the contract `sr::denoise_inputs` already states.
/// A texture left empty is one this call does not carry; the implementation refuses when NGX requires it.
struct dlss_eval_desc
{
    sg::texture_2d color;
    sg::texture_2d albedo;
    sg::texture_2d specular_albedo;
    sg::texture_2d normal;
    sg::texture_2d roughness;
    sg::texture_2d depth;
    sg::texture_2d motion;
    sg::texture_2d output;

    /// This frame's sub-pixel offset of the primary rays, in input pixels.
    tg::vec2f jitter = tg::vec2f(0, 0);

    /// Whether this call starts from no history — a first call, a new extent, or a camera cut.
    bool reset = false;

    /// The multiplier the caller will apply before display, which is how NGX judges how bright a pixel ends up.
    f32 exposure = 1.0f;
};

/// Whether Ray Reconstruction can run here: compiled in, dx12, and the driver reporting the feature available.
///
/// Answered from the open instance when the device has one.
/// Otherwise NGX is probed and shut down again, and the answer cached per adapter: it is a property of the adapter and
/// driver rather than of a device, and an adapter's LUID is never reused the way a freed device's address is.
[[nodiscard]] bool dlss_is_available(sg::context const& ctx);

/// Opens NGX for `ctx`'s device and keeps its capability map; null where it cannot run.
///
/// A device already open hands back the same record, counted, so an evicted routine's pending close cannot shut down
/// the instance its replacement opened.
[[nodiscard]] std::shared_ptr<dlss_instance> dlss_open(sg::context& ctx);

/// Gives up one `dlss_open`; the last one shuts NGX down for the device.
///
/// **The GPU must be done with every stream of it**, so a caller registers this through `ctx.defer_until_retired`
/// rather than calling it directly.
/// The record itself lives on while a stream holds it, marked closed.
void dlss_close(std::shared_ptr<dlss_instance> const& instance);

/// Creates one stream: its parameter map, and its feature, whose initialization is recorded onto `cmd`.
/// Null when it could not be created, which is logged on sr's domain.
[[nodiscard]] dlss_stream* dlss_create_stream(sg::command_list& cmd,
                                              std::shared_ptr<dlss_instance> const& instance,
                                              dlss_feature_desc const& desc);

/// Releases the feature and the parameter map, then deletes `stream`; null is a no-op.
///
/// **The GPU must be done with it.** NGX frees device memory here, and a stream released while a frame that used it
/// is still in flight is a use-after-free with no diagnostic — so it runs through `ctx.defer_until_retired`.
/// A stream whose instance was already closed is a broken `sr::denoise_history` contract: that is logged as an error,
/// and the struct is deleted without calling NGX.
void dlss_release_stream(dlss_stream* stream);

/// How many streams `dlss_release_stream` has released, for a test to observe when that happens.
[[nodiscard]] int dlss_released_stream_count();

/// Records one evaluation onto `cmd`, inside a native scope that barriers every resource it names.
/// False when NGX refused it, which is logged.
[[nodiscard]] bool dlss_evaluate(sg::command_list& cmd, dlss_stream& stream, dlss_eval_desc const& desc);
} // namespace sr::impl
