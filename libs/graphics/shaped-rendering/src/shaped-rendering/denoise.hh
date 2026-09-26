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

/// Denoising — and, once a member supports it, upscaling — behind one call.
///
/// `sr::denoise_routine` is the front: a caller names a method (or `automatic`) and the front forwards to the
/// member routine that implements it.
/// Every member is also a routine of its own, callable directly with its full options.
/// libs/graphics/shaped-rendering/docs/denoising.md is the design: which members exist, how they meet a progressive path tracer, and why.

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
/// Named presets only: each member maps them onto a ratio it supports, and a caller asks `denoise_input_extent` for
/// the size to trace rather than computing one — so no caller can ask for a ratio a member would reject.
/// A spatial member supports exactly 1, and answers every preset with the output's own extent.
enum class sr::render_scale_preset : sg::u8
{
    native,
    quality,
    balanced,
    performance,
};

/// One guide buffer a member may read beside the noisy color.
enum class sr::denoise_guide : sg::u8
{
    albedo,          ///< diffuse reflectance at the primary hit
    specular_albedo, ///< specular reflectance at the primary hit
    normal,          ///< world-space shading normal at the primary hit, in rgb
    roughness,       ///< perceptual roughness at the primary hit, in r
    depth,           ///< linear view depth of the primary hit, in r; 0 or less where a ray missed
    motion,          ///< this frame's pixel minus last frame's, in input pixels, in rg
    hit_distance,    ///< distance to the first secondary hit, in r

    split_diffuse_specular, ///< radiance arrives as two textures (`color` diffuse, `specular` specular) rather than one
};
CC_FLAG_ENUM_INDEXED(sr, denoise_guide, u16);

namespace sr
{
using denoise_guide_set = cc::flags<denoise_guide>;
}

/// The knobs a caller sets once for every member.
///
/// Flat on purpose: each field is named for what it does, not for who reads it, and says which members read it.
/// A member ignores what it has no use for, so switching members keeps every knob that still means something.
/// A member's own options — the full vendor surface — are on the member routine, never here.
struct sr::denoise_settings
{
    /// `automatic` because a caller reaching this struct wants something denoised; a caller holding a setting that is
    /// off by default (sv's per-layer one) says `none` itself.
    denoise_method method = denoise_method::automatic;
    render_scale_preset scale = render_scale_preset::native;

    /// Whether this call carries fresh per-frame samples — this frame's own, with motion vectors — rather than a
    /// converging mean.
    /// It lives here rather than beside each call so that planning and running a frame cannot disagree about it:
    /// `denoise_input_extent`, `resolve_denoise_method` and `denoise_routine::execute` all read this one answer.
    /// False means `automatic` picks among the spatial members only, since a temporal member's history would
    /// double-count what the mean already averaged.
    bool fresh_samples = false;

    /// Every member reads this.
    /// atrous and svgf: the number of wavelet passes (3, 4, 5).
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
    /// The vendor members judge noise by how bright a pixel ends up on screen, and assume 1 without it.
    f32 exposure = 1.0f;
};

/// Everything beside the noisy color that a member may read.
///
/// All of it is at the INPUT extent and in input pixels; only `denoise_inputs::output` is at the output's.
/// A texture left empty is a guide the caller does not have; a member that requires it reports `unsupported`.
struct sr::denoise_guides
{
    sg::texture_2d albedo;
    sg::texture_2d specular_albedo;
    sg::texture_2d normal;
    sg::texture_2d roughness;
    sg::texture_2d depth;
    sg::texture_2d motion;
    sg::texture_2d hit_distance;

    /// This frame's sub-pixel offset of the primary rays, in input pixels, in [-0.5, 0.5].
    tg::vec2f jitter = tg::vec2f(0, 0);

    tg::mat4f view_to_clip = tg::mat4f::identity;
    tg::mat4f previous_view_to_clip = tg::mat4f::identity;
};

/// One denoise call's images.
struct sr::denoise_inputs
{
    /// The noisy radiance, linear and HDR, at the input extent.
    /// Diffuse radiance alone when the guides carry `split_diffuse_specular`.
    /// No member reads its alpha; every member copies it into `output`.
    sg::texture_2d color;

    /// Specular radiance, only under `split_diffuse_specular`.
    sg::texture_2d specular;

    denoise_guides guides;

    /// Where the result goes: needs `image` usage, and must not be `color`.
    /// Its extent is the output extent; any ratio to the input other than 1 must be one `denoise_input_extent` produced.
    ///
    /// Its rgb is the denoised radiance and **its alpha is `color`'s, carried through untouched** — every member
    /// keeps it rather than writing one of its own, so switching members never changes what a caller composites with.
    /// Whether a vendor member can honour that is open; see libs/graphics/shaped-rendering/docs/denoising.md.
    sg::texture_2d output;

    /// How many samples per pixel `color` already averages — an accumulated mean passes its frame count times its
    /// per-frame samples.
    /// Spatial members back off as it grows, so a converged image is left alone; 0 means one.
    u32 sample_count = 0;

    /// The guides this call carries, derived from which textures are set — `specular` included.
    [[nodiscard]] denoise_guide_set present_guides() const;
};

/// What one denoise call did.
enum class sr::denoise_status : sg::u8
{
    denoised,    ///< `output` holds the result
    pending,     ///< the member is still initializing; `output` untouched
    unsupported, ///< this build or device cannot run the requested member, or the inputs lack a guide it requires
    failed,      ///< the member failed to initialize; `output` untouched until a reload
};

struct sr::denoise_outcome
{
    denoise_status status = denoise_status::pending;

    /// The member that ran, or was asked to.
    denoise_method method = denoise_method::none;

    /// Whether the call started from no history — a first call, a new extent, a new member, or a `reset`.
    bool restarted = false;

    [[nodiscard]] bool is_denoised() const { return status == denoise_status::denoised; }
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
/// It holds images and nothing else.
/// A member needing state that is not a texture — a vendor feature handle, which dlss_rr and fsr_rr both take — is
/// what replaces the fixed array with a per-member state object; see libs/graphics/shaped-rendering/docs/denoising.md.
class sr::denoise_history
{
public:
    denoise_history() = default;
    denoise_history(denoise_history&&) noexcept = default;
    denoise_history& operator=(denoise_history&&) noexcept = default;
    denoise_history(denoise_history const&) = delete;
    denoise_history& operator=(denoise_history const&) = delete;

    /// How many images a member may keep here.
    /// Public because each member asserts its own slot range at namespace scope, where friendship does not reach.
    static constexpr int state_slots = 8;

    /// Makes the next call start from no history, as on a camera cut.
    /// The textures are kept and overwritten, since a cut does not change their size.
    void reset() { _reset_requested = true; }

    /// The member that built what this holds, or `none` while empty.
    [[nodiscard]] denoise_method method() const { return _method; }

    /// The input extent this was built for, or 0x0 while empty.
    [[nodiscard]] tg::vec2i extent() const { return _extent; }

private:
    friend class atrous_denoise_routine;
    friend class svgf_denoise_routine;

    /// Brings this to `method` at `extent`, dropping everything if either changed.
    /// Returns whether the call starts from no history.
    bool _prepare(denoise_method method, tg::vec2i extent);

    denoise_method _method = denoise_method::none;
    tg::vec2i _extent = tg::vec2i(0, 0);
    bool _reset_requested = false;

    /// How many calls this history has seen since it was last built, which is what a temporal member ping-pongs on.
    u32 _frame = 0;

    /// The images a member keeps from call to call — its history and its scratch — so a steady stream allocates nothing.
    /// Which slot holds what is the member's own business.
    cc::fixed_array<sg::texture_2d, state_slots> _state;
};

/// Which members this context can run.
struct sr::denoise_support
{
    bool atrous = false;
    bool svgf = false;
    bool oidn = false;
    bool dlss_rr = false;
    bool fsr_rr = false;

    [[nodiscard]] bool supports(denoise_method m) const;
};

namespace sr
{
/// The member's name, for a log line or a UI label.
/// Stable: these are what `sr::denoise_method` spells, not prose.
[[nodiscard]] cc::string_view to_string(denoise_method m);

/// What a call did, for the same use.
[[nodiscard]] cc::string_view to_string(denoise_status s);

/// Which members `ctx` can run: compiled in, buildable by the shader library this process registered, and present
/// on its device.
/// Cheap.
/// The native members are HLSL, so their answer depends on the compilers the library has — adding one can change it.
///
/// A supported member can still be `pending` for its first frames, and `failed` if its shader does not build.
[[nodiscard]] denoise_support query_denoise_support(sg::context const& ctx);

/// The member `settings.method` resolves to on `ctx`: itself when named, the best supported one for `automatic`.
/// `none` when nothing is supported or nothing was asked for.
///
/// A named member resolves to itself whether or not `ctx` supports it, so a comparison between two named members
/// never silently compares one with itself; refusing it is `denoise_routine::execute`'s job.
[[nodiscard]] denoise_method resolve_denoise_method(sg::context const& ctx, denoise_settings const& settings);

/// Whether a member reads history, and so needs fresh per-frame samples and motion vectors rather than a converging mean.
[[nodiscard]] bool is_temporal(denoise_method m);

/// The guides `m` requires; a call missing one reports `unsupported`.
[[nodiscard]] denoise_guide_set required_guides(denoise_method m);

/// The guides `m` reads when they are there.
[[nodiscard]] denoise_guide_set optional_guides(denoise_method m);

/// The input extent to trace so that the member `settings` resolves to produces `output_extent` under `settings.scale`.
///
/// Always ask this rather than scaling by hand: a member supports only its own ratios, and a spatial one only 1.
/// A member `ctx` cannot run answers `output_extent`, because the call will be refused and a caller that traced
/// smaller for it would composite a smaller image into its own output.
[[nodiscard]] tg::vec2i denoise_input_extent(sg::context const& ctx,
                                             denoise_settings const& settings,
                                             tg::vec2i output_extent);
} // namespace sr

/// The front routine: one call for every denoiser.
///
/// Resolves `settings.method` against this context, then forwards to the member's own routine.
/// Members are acquired when the call runs rather than through dependency tokens, so a member this machine cannot
/// initialize never holds the front pending.
/// Prewarming the front prewarms every supported member, so their shaders compile before the first call.
class sr::denoise_routine : public sg::render_routine<denoise_routine>
{
public:
    /// Denoises `in.color` into `in.output`, carrying `history` from call to call.
    ///
    /// Nothing is written unless the outcome is `denoised`, so a caller composites the raw image otherwise.
    [[nodiscard]] static denoise_outcome execute(sg::command_list& cmd,
                                                 denoise_inputs const& in,
                                                 denoise_history& history,
                                                 denoise_settings const& settings);

protected:
    cc::shared_async<cc::unit> init(sg::routine_init_scope scope) override;
};
