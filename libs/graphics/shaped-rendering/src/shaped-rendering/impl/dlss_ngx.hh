#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// The NGX seam: everything `sr::dlss_rr_routine` needs from NVIDIA's SDK, with none of its headers.
///
/// Two implementations, chosen by the build: `dlss_ngx.cc` where the SDK was fetched and dx12 exists, `dlss_null.cc`
/// otherwise.
/// That is what lets the routine and its whole public surface exist unconditionally — a caller naming `dlss_rr` always
/// compiles, and only the answer changes.
///
/// Everything here is dx12-only, because a vendor member needs `sg::backend::dx12::dx12_native_scope` to hand foreign
/// code a command list and no other backend has one.
namespace sr::impl
{
/// What one Ray Reconstruction feature is created for.
///
/// The extents are fixed at creation: NGX builds its networks for a size, so a resize is a new feature rather than a
/// parameter, which is exactly why `denoise_history` drops one when its extent moves.
struct dlss_feature_desc
{
    tg::vec2i input_extent = tg::vec2i(0, 0);
    tg::vec2i output_extent = tg::vec2i(0, 0);

    /// 0 fastest, 1 balanced, 2 best — mapped onto NGX's own performance/quality presets by the implementation.
    int quality = 1;

    /// Whether the radiance handed over is linear HDR, which ours is.
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

/// Whether Ray Reconstruction can run here: compiled in, dx12, NGX initialized against this device, and the driver
/// reporting the feature available.
///
/// Answered once per context and cached, since it costs an NGX round trip and cannot change while a device lives.
[[nodiscard]] bool dlss_is_available(sg::context const& ctx);

/// Creates one stream's feature, recording its initialization onto `cmd`.
/// Null when it could not be created, which is logged once on sr's domain.
[[nodiscard]] void* dlss_create_feature(sg::command_list& cmd, dlss_feature_desc const& desc);

/// Releases what `dlss_create_feature` returned; null is a no-op.
///
/// **The GPU must be done with it.** NGX frees device memory here, and a feature released while a frame that used it
/// is still in flight is a use-after-free with no diagnostic — see `sr::denoise_history`, which is where the lifetime
/// actually lives.
void dlss_release_feature(void* feature);

/// Records one evaluation onto `cmd`, inside a native scope that barriers every resource it names.
/// False when NGX refused it, which is logged once.
[[nodiscard]] bool dlss_evaluate(sg::command_list& cmd, void* feature, dlss_eval_desc const& desc);
} // namespace sr::impl
