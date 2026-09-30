#pragma once

#include <clean-core/common/flags.hh>
#include <clean-core/container/fixed_array.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-graphics/routine/render_routine.hh>
#include <shaped-rendering/fwd.hh>
#include <typed-geometry/linalg/mat.hh>
#include <typed-geometry/linalg/vec.hh>

#include <memory>

/// Reconstruction — denoising, then upscaling — behind one call.
///
/// `sr::reconstruct_routine` is the front: a caller names a denoiser and an upscaler (or `automatic`) and the front
/// forwards to the member routines that implement them, the denoiser at the traced extent and the upscaler after it.
/// Every member is also a routine of its own, callable directly with its full options.
/// libs/graphics/shaped-rendering/docs/reconstruction.md is the design: which members exist, how they meet a progressive path tracer, and why.

/// Which denoiser runs.
///
/// A member that this build or this device cannot run reports `unsupported` rather than falling through to another:
/// only `automatic` chooses, so a comparison between two named members never silently compares one with itself.
enum class sr::denoise_method : sg::u8
{
    none,
    automatic, ///< the best member this context supports, preferring the temporal ones while a caller runs temporally

    atrous,  ///< edge-avoiding à-trous wavelet filter; spatial, native
    svgf,    ///< à-trous plus reprojected history; temporal, native
    oidn,    ///< Intel Open Image Denoise; spatial
    dlss_rr, ///< NVIDIA DLSS Ray Reconstruction; temporal
    fsr_rr,  ///< AMD FSR Ray Regeneration; temporal
    nrd,     ///< NVIDIA Real-Time Denoisers (REBLUR); temporal, and runs on any dx12 adapter

    count_
};

/// The coarse quality knob every member reads, mapped onto its own presets.
enum class sr::denoise_quality : sg::u8
{
    fast,
    balanced,
    best,
};

/// How much smaller than the output the caller traces.
///
/// Named presets only: each member maps them onto a ratio it supports, and a caller asks `reconstruct_input_extent` for
/// the size to trace rather than computing one — so no caller can ask for a ratio a member would reject.
/// A denoiser that does not upscale runs at the traced extent, and the upscaler behind it maps the preset instead.
enum class sr::render_scale_preset : sg::u8
{
    native,
    quality,
    balanced,
    performance,
};

/// Which upscaler runs behind the denoiser.
///
/// An upscaler does not denoise — a temporal upscaler smears Monte Carlo noise rather than removing it — so it runs on
/// a denoiser's output, or directly on an image that has no noise to begin with.
/// As with `denoise_method`, a named upscaler this build or device cannot run reports `unsupported`, and only
/// `automatic` chooses.
enum class sr::upscale_method : sg::u8
{
    none,
    automatic, ///< the best upscaler this context supports, and none while the scale is native

    fsr, ///< AMD FSR 3.1's analytic upscaler; runs on any GPU

    count_
};

/// One guide buffer a member may read beside the noisy color.
enum class sr::reconstruct_guide : sg::u8
{
    /// Diffuse reflectance at the primary hit, so zero on a metal.
    /// A member reading split radiance reads it and `specular_albedo` separately; one reading unsplit radiance
    /// demodulates by their sum.
    albedo,
    specular_albedo, ///< specular reflectance at the primary hit
    normal,          ///< world-space shading normal at the primary hit, in rgb
    roughness,       ///< perceptual roughness at the primary hit, in r
    depth,           ///< linear view depth of the primary hit, in r; 0 or less where a ray missed
    /// The surface's own motion, in input pixels, in rg: where this frame's sample was traced minus where that
    /// surface was last frame, so a still camera reports zero whatever the jitter.
    motion,
    hit_distance, ///< distance to the first secondary hit, in r

    split_diffuse_specular, ///< radiance arrives as two textures (`color` diffuse, `specular` specular) rather than one
};
CC_FLAG_ENUM_INDEXED(sr, reconstruct_guide, u16);

namespace sr
{
using reconstruct_guide_set = cc::flags<reconstruct_guide>;
}

/// The knobs a caller sets once for every member.
///
/// Flat on purpose: each field is named for what it does, not for who reads it, and says which members read it.
/// A member ignores what it has no use for, so switching members keeps every knob that still means something.
/// A member's own options — the full vendor surface — are on the member routine, never here.
struct sr::reconstruct_settings
{
    /// `automatic` because a caller reaching this struct wants something denoised; a caller holding a setting that is
    /// off by default (sv's per-layer one) says `none` itself.
    denoise_method denoiser = denoise_method::automatic;

    /// Runs behind the denoiser whenever the input extent is smaller than the output's.
    /// Ignored for a denoiser that upscales by itself (`dlss_rr`, `fsr_rr`).
    /// `none` with a denoiser of `none` is a call with nothing to do; either one alone is fine.
    upscale_method upscaler = upscale_method::automatic;
    render_scale_preset scale = render_scale_preset::native;

    /// Whether this call carries fresh per-frame samples — this frame's own, with motion vectors — rather than a
    /// converging mean.
    /// It lives here rather than beside each call so that planning and running a frame cannot disagree about it:
    /// `reconstruct_input_extent`, `resolve_denoise_method` and `reconstruct_routine::execute` all read this one answer.
    /// False means `automatic` picks among the spatial members only, since a temporal member's history would
    /// double-count what the mean already averaged.
    bool fresh_samples = false;

    /// Every member reads this.
    /// atrous and svgf: the number of wavelet passes (3, 4, 5).
    /// nrd: how long REBLUR's two histories may grow.
    /// oidn: `fast` runs its small network, the others its base one.
    denoise_quality quality = denoise_quality::balanced;

    /// In [0, 1]; higher keeps more detail and removes less noise.
    /// atrous and svgf: how strictly a neighbour must match in luminance to be averaged in.
    f32 sharpness = 0.5f;

    /// In [0, 1]: 1 gives each new frame at least half the weight, and is noisier.
    /// svgf only; the vendor members decide this themselves.
    f32 temporal_responsiveness = 0.2f;

    /// Whether the albedo and normal guides carry noise of their own (depth of field, motion blur) and should be
    /// filtered first.
    /// oidn only.
    bool noisy_guides = false;

    /// A multiplier the caller will apply to the image before display.
    /// dlss_rr judges noise by how bright a pixel ends up on screen and assumes 1 without it; the fsr upscaler reads it
    /// the same way, for the history it keeps.
    ///
    /// NRD is the member this does NOT reach, and deliberately: its input contract says radiance must not be
    /// premultiplied by an exposure, so it is handed the radiance the tracer produced.
    f32 exposure = 1.0f;

    /// In [0, 1]; 0 is off.
    /// Sharpening applied to the upscaled image; fsr reads it (RCAS).
    f32 upscale_sharpness = 0.0f;

    /// The time since the previous call, in milliseconds; fsr scales a few of its decay rates by it.
    f32 frame_time_ms = 1000.0f / 60.0f;
};

/// Everything beside the noisy color that a member may read.
///
/// All of it is at the INPUT extent and in input pixels; only `reconstruct_inputs::output` is at the output's.
/// A texture left empty is a guide the caller does not have; a member that requires it reports `unsupported`.
struct sr::reconstruct_guides
{
    sg::texture_2d albedo;
    sg::texture_2d specular_albedo;
    sg::texture_2d normal;
    sg::texture_2d roughness;
    sg::texture_2d depth;
    sg::texture_2d motion;
    sg::texture_2d hit_distance;

    /// This frame's sub-pixel offset of the primary rays, in input pixels, in [-0.5, 0.5].
    /// One offset for every sample of the frame while an upscaler runs, taken from `sr::reconstruct_jitter`.
    tg::vec2f jitter = tg::vec2f(0, 0);
    tg::vec2f previous_jitter = tg::vec2f(0, 0);

    tg::mat4f view_to_clip = tg::mat4f::identity;
    tg::mat4f previous_view_to_clip = tg::mat4f::identity;

    /// The camera itself, which a member reprojecting in world space needs beside the projection.
    /// nrd only; the other members work from the motion guide alone.
    tg::mat4f world_to_view = tg::mat4f::identity;
    tg::mat4f previous_world_to_view = tg::mat4f::identity;
};

/// One denoise call's images.
struct sr::reconstruct_inputs
{
    /// The noisy radiance, linear and HDR, at the input extent.
    /// Diffuse radiance alone when the guides carry `split_diffuse_specular`.
    /// No member reads its alpha; every member copies it into `output`.
    sg::texture_2d color;

    /// Specular radiance, only under `split_diffuse_specular`.
    sg::texture_2d specular;

    reconstruct_guides guides;

    /// Where the result goes: needs `image` usage, and must not be `color`.
    /// Its extent is the output extent; any ratio to the input other than 1 must be one `reconstruct_input_extent` produced.
    ///
    /// Its rgb is the denoised radiance and **its alpha is `color`'s, carried through untouched** — every denoise
    /// member keeps it rather than writing one of its own, so switching members never changes what a caller composites with.
    /// An upscaled output is the exception: its alpha is the upscaler's own, since `color`'s is at the other extent.
    sg::texture_2d output;

    /// How many samples per pixel `color` already averages — an accumulated mean passes its frame count times its
    /// per-frame samples.
    /// Spatial members back off as it grows, so a converged image is left alone; 0 means one.
    u32 sample_count = 0;

    /// The guides this call carries, derived from which textures are set — `specular` included.
    [[nodiscard]] reconstruct_guide_set present_guides() const;
};

/// What one denoise call did.
enum class sr::reconstruct_status : sg::u8
{
    denoised,    ///< `output` holds the result
    pending,     ///< the member is still initializing; `output` untouched
    unsupported, ///< this build or device cannot run the requested member, or the inputs lack a guide it requires
    failed,      ///< the member failed to initialize; `output` untouched until a reload
};

struct sr::reconstruct_outcome
{
    reconstruct_status status = reconstruct_status::pending;

    /// The member that ran, or was asked to.
    denoise_method denoiser = denoise_method::none;

    /// The upscaler that ran behind it, or was asked to; `none` when the call did not upscale.
    upscale_method upscaler = upscale_method::none;

    /// Whether the call started from no history — a first call, a new extent, a new member, or a `reset`.
    bool restarted = false;

    [[nodiscard]] bool is_denoised() const { return status == reconstruct_status::denoised; }
};

/// One upscale call's images, in sr's guide conventions.
///
/// Everything but `output` is at the INPUT extent and in input pixels.
struct sr::upscale_inputs
{
    /// The image to upscale: linear and HDR, and already clean.
    sg::texture_2d color;

    /// Linear view depth of the primary hit, in r; 0 or less where a ray missed.
    sg::texture_2d depth;

    /// The surface's own motion, in input pixels, in rg: where this frame's sample was traced minus where that
    /// surface was last frame, so a still camera reports zero whatever the jitter.
    sg::texture_2d motion;

    /// This frame's sub-pixel offset of every sample, in input pixels, in [-0.5, 0.5].
    /// One offset for the whole frame: an upscaler places each pixel's sample by it, so a tracer that jitters each
    /// sample on its own gives it nothing to reconstruct from.
    tg::vec2f jitter = tg::vec2f(0, 0);

    /// The camera's projection, read for its vertical field of view.
    tg::mat4f view_to_clip = tg::mat4f::identity;

    /// A multiplier the caller will apply to the image before display.
    f32 exposure = 1.0f;

    /// Where the result goes, at the output extent: needs `image` usage, and must not be `color`.
    /// Its alpha is the upscaler's own, not `color`'s.
    sg::texture_2d output;
};

/// What one upscale call did.
struct sr::upscale_outcome
{
    /// `denoised` means `output` holds the result, as it does for the front.
    reconstruct_status status = reconstruct_status::pending;

    /// Whether the call started from no history — a first call, a new extent, or a `reset`.
    bool restarted = false;
};

/// Everything an upscaler keeps between calls, for one image stream.
///
/// Owned by the caller, one per stream, for the reasons `reconstruct_history` is, and move-only for the same one.
/// Empty until the first call, and rebuilt when either extent changes.
///
/// FSR 3.1 keeps two full images at the output extent and about a dozen at the input extent, most of them small:
/// roughly 80 MiB for 720p in and 1080p out.
class sr::upscale_history
{
public:
    upscale_history() = default;
    upscale_history(upscale_history&&) noexcept = default;
    upscale_history& operator=(upscale_history&&) noexcept = default;
    upscale_history(upscale_history const&) = delete;
    upscale_history& operator=(upscale_history const&) = delete;

    /// Makes the next call start from no history, as on a camera cut.
    void reset() { _reset_requested = true; }

    /// Whether a `reset` is waiting for the next call to consume it.
    [[nodiscard]] bool is_reset_pending() const { return _reset_requested; }

    /// The extents this was built for, or 0x0 while empty.
    [[nodiscard]] tg::vec2i input_extent() const { return _input_extent; }
    [[nodiscard]] tg::vec2i output_extent() const { return _output_extent; }

private:
    friend class fsr_upscale_routine;
    friend class reconstruct_routine;

    /// Brings this to the two extents, keeping the upscaler's state only if neither changed.
    /// Returns whether the call starts from no history, and consumes a pending `reset`.
    bool _prepare(tg::vec2i input_extent, tg::vec2i output_extent);

    /// The upscaler's own per-stream object — for FSR, its context and every image it created.
    /// Type-erased so this header names no SDK type; it must hold only what is safe to drop mid-frame.
    std::shared_ptr<void> _state;

    tg::vec2i _input_extent = tg::vec2i(0, 0);
    tg::vec2i _output_extent = tg::vec2i(0, 0);
    bool _reset_requested = false;

    /// The denoiser whose output the front last upscaled, so that switching it restarts this history too.
    denoise_method _source_denoiser = denoise_method::none;
};

/// Everything a denoiser keeps between calls, for one image stream.
///
/// Owned by the caller, one per stream, and passed to every call: a routine is a per-context singleton and cannot
/// know which stream a call belongs to, or when a stream has gone.
/// Holding one per stream is what keeps two views from sharing history, and dropping it is what frees the history.
///
/// Move-only: copying would fork a history, and both copies would then believe they are the continuation.
/// Empty until the first call; a call that sees a different extent or member rebuilds it and reports `restarted`.
///
/// **A temporal member's history is large**: svgf holds eight full-screen images, six of them `rgba32_float` and two
/// `rg32_float`, which is about 221 MiB at 1080p and 886 MiB at 2160p — per stream.
/// Dropping the history of a view nobody is looking at is how a caller gets that back, and is what a caller with many
/// views should do.
///
/// It holds images, plus at most one object of the member's own for state that is not a texture.
class sr::reconstruct_history
{
public:
    reconstruct_history() = default;
    reconstruct_history(reconstruct_history&&) noexcept = default;
    reconstruct_history& operator=(reconstruct_history&&) noexcept = default;
    reconstruct_history(reconstruct_history const&) = delete;
    reconstruct_history& operator=(reconstruct_history const&) = delete;

    /// How many images a member may keep here.
    /// Public because each member asserts its own slot range at namespace scope, where friendship does not reach.
    static constexpr int state_slots = 8;

    /// Makes the next call start from no history, as on a camera cut — the denoiser's and the upscaler's alike.
    /// The textures are kept and overwritten, since a cut does not change their size.
    void reset()
    {
        _reset_requested = true;
        _upscale.reset();
    }

    /// Whether a `reset` is waiting for the next call to consume it.
    [[nodiscard]] bool is_reset_pending() const { return _reset_requested; }

    /// The member that built what this holds, or `none` while empty.
    [[nodiscard]] denoise_method denoiser() const { return _method; }

    /// The input extent this was built for, or 0x0 while empty.
    [[nodiscard]] tg::vec2i extent() const { return _extent; }

private:
    friend class atrous_denoise_routine;
    friend class svgf_denoise_routine;
    friend class nrd_denoise_routine;
    friend class oidn_denoise_routine;
    friend class reconstruct_routine;

    /// Brings this to `method` at `extent`, dropping everything if either changed.
    /// Returns whether the call starts from no history.
    bool _prepare(denoise_method method, tg::vec2i extent);

    /// A member's own per-stream object — for OIDN the network and its feature maps, for NRD its instance.
    ///
    /// Type-erased so this header names no member's type; `make_shared` captured the deleter that frees it.
    /// It must hold only what is safe to drop mid-frame, as sg resources are.
    /// `_prepare` drops it whenever it drops the rest, since the state is built for one extent.
    std::shared_ptr<void> _member_state;

    denoise_method _method = denoise_method::none;
    tg::vec2i _extent = tg::vec2i(0, 0);
    bool _reset_requested = false;

    /// How many calls this history has seen since it was last built, which is what a temporal member ping-pongs on.
    u32 _frame = 0;

    /// The images a member keeps from call to call — its history and its scratch — so a steady stream allocates nothing.
    /// Which slot holds what is the member's own business.
    cc::fixed_array<sg::texture_2d, state_slots> _state;

    /// The upscaler's history, kept apart from the denoiser's so that neither one's rebuild drops the other.
    upscale_history _upscale;

    /// The denoiser's output at the input extent, which the upscaler then reads.
    sg::texture_2d _upscale_source;
};

/// Which members this context can run.
struct sr::reconstruct_support
{
    bool atrous = false;
    bool svgf = false;
    bool oidn = false;
    bool dlss_rr = false;
    bool fsr_rr = false;
    bool nrd = false;

    bool fsr = false;

    [[nodiscard]] bool supports(denoise_method m) const;
    [[nodiscard]] bool supports(upscale_method m) const;
};

namespace sr
{
/// The member's name, for a log line or a UI label.
/// Stable: these are what `sr::denoise_method` spells, not prose.
[[nodiscard]] cc::string_view to_string(denoise_method m);

/// The upscaler's name, for the same use.
[[nodiscard]] cc::string_view to_string(upscale_method m);

/// What a call did, for the same use.
[[nodiscard]] cc::string_view to_string(reconstruct_status s);

/// Which members `ctx` can run: compiled in, buildable by the shader library this process registered, and present
/// on its device.
/// Cheap.
/// The native members are HLSL, so their answer depends on the compilers the library has — adding one can change it.
///
/// A supported member can still be `pending` for its first frames, and `failed` if its shader does not build.
[[nodiscard]] reconstruct_support query_reconstruct_support(sg::context const& ctx);

/// The member `settings.denoiser` resolves to on `ctx`: itself when named, the best supported one for `automatic`.
/// `none` when nothing is supported or nothing was asked for.
///
/// A named member resolves to itself whether or not `ctx` supports it, so a comparison between two named members
/// never silently compares one with itself; refusing it is `reconstruct_routine::execute`'s job.
[[nodiscard]] denoise_method resolve_denoise_method(sg::context const& ctx, reconstruct_settings const& settings);

/// Whether a member reads history, and so needs fresh per-frame samples and motion vectors rather than a converging mean.
[[nodiscard]] bool is_temporal(denoise_method m);

/// The guides `m` requires; a call missing one reports `unsupported`.
[[nodiscard]] reconstruct_guide_set required_guides(denoise_method m);

/// The guides `m` reads when they are there.
[[nodiscard]] reconstruct_guide_set optional_guides(denoise_method m);

/// The upscaler `settings.upscaler` resolves to on `ctx`, behind the denoiser `settings.denoiser` resolves to.
///
/// `none` when the denoiser upscales by itself, and for `automatic` while `settings.scale` is native or nothing is
/// supported.
/// A named upscaler resolves to itself whether or not `ctx` supports it, and runs even at a native scale — FSR then
/// anti-aliases rather than upscales.
[[nodiscard]] upscale_method resolve_upscale_method(sg::context const& ctx, reconstruct_settings const& settings);

/// The input extent to trace so that the members `settings` resolves to produce `output_extent` under `settings.scale`.
///
/// Always ask this rather than scaling by hand: each upscaler supports only its own ratios, and a denoiser with no
/// upscaler behind it only 1.
/// A member `ctx` cannot run answers `output_extent`, because the call will be refused and a caller that traced
/// smaller for it would composite a smaller image into its own output.
[[nodiscard]] tg::vec2i reconstruct_input_extent(sg::context const& ctx,
                                                 reconstruct_settings const& settings,
                                                 tg::vec2i output_extent);

/// The sub-pixel offset to trace frame `frame_index` at, in input pixels in [-0.5, 0.5], for `reconstruct_guides::jitter`.
///
/// Every sample of the frame takes this one offset, in place of a random position inside its pixel: an upscaler
/// reconstructs from where each frame's samples landed, so it has to know.
/// (0, 0) whenever nothing upscales, so a caller may always trace with it.
/// `frame_index` counts the caller's frames and may wrap; the sequence repeats after a period set by the ratio.
[[nodiscard]] tg::vec2f reconstruct_jitter(sg::context const& ctx,
                                           reconstruct_settings const& settings,
                                           tg::vec2i output_extent,
                                           u32 frame_index);
} // namespace sr

/// The front routine: one call for every denoiser and upscaler.
///
/// Resolves `settings.denoiser` and `settings.upscaler` against this context, then forwards to the members' own routines:
/// the denoiser into a scratch image at the input extent, and the upscaler from there into the output.
/// With no upscaler it runs the denoiser alone, straight into the output.
/// Members are acquired when the call runs rather than through dependency tokens, so a member this machine cannot
/// initialize never holds the front pending.
/// Prewarming the front prewarms every supported member, so their shaders compile before the first call.
class sr::reconstruct_routine : public sg::render_routine<reconstruct_routine>
{
public:
    /// Denoises `in.color` and upscales it into `in.output`, carrying `history` from call to call.
    ///
    /// Nothing is written unless the outcome is `denoised`, so a caller composites the raw image otherwise.
    /// An upscaler requires the depth and motion guides.
    [[nodiscard]] static reconstruct_outcome execute(sg::command_list& cmd,
                                                     reconstruct_inputs const& in,
                                                     reconstruct_history& history,
                                                     reconstruct_settings const& settings);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;
};
