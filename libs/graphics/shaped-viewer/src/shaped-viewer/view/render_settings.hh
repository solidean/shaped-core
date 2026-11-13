#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-rendering/display.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/fwd.hh>

namespace sv
{
/// The samples per pixel where a layer stops accumulating, which bounds GPU LOAD rather than the estimate.
///
/// Nothing ages out below it: an rgba32_float target still moves its mean at a weight of 1 / (n + 1) here, so the estimate
/// would go on improving for as long as anyone will watch it.
/// What stops it is that a viewer sits open far longer than anyone looks at it, and a window nobody is watching may not hold
/// the GPU busy forever for a mean that is already past what a display can show.
/// A layer sitting at the cap is therefore **converged as far as it will ever get**, which is why the cap is public:
/// a caller waiting for a settled image has to know the difference between "not there yet" and "this is as good as it
/// gets", and a target above the cap would otherwise wait forever.
/// `view_ref::is_accumulation_converged` is the answer that already applies it.
///
/// Samples rather than frames, since what a frame holds is the integrator's and the GPU's: a megakernel frame holds
/// `samples_per_pixel`, and a wavefront frame whatever its rounds finish.
/// The wavefront starts no sample past it, and the megakernel traces nothing once its frames reach it, so a still view there costs no trace.
///
/// Per view rather than per process is the intended shape: `render_settings::max_samples_per_pixel`, excluded from the trace hash.
/// issue: feat-sv-max-samples-per-pixel
inline constexpr u32 accumulation_sample_cap = 16384;

/// The mode a layer asking for `mode` is drawn in on `ctx`: `mode` itself, unless it is `automatic`.
[[nodiscard]] render_mode resolve_render_mode(sg::context const& ctx, render_mode mode);
} // namespace sv

/// How a layer's image is produced from its scene: traced by one of two path tracers, or rasterized.
///
/// The two tracers converge to the same image; they differ in what a frame costs and how a moving image looks.
/// Neither wins everywhere: on an RTX laptop GPU the megakernel is about twice as fast on a shallow scene, and the wavefront far ahead wherever paths run deep, as in a scattering medium.
/// On a Radeon 780M the wavefront wins the shallow scene too; libs/graphics/shaped-viewer/docs/wavefront.md has the measurements.
enum class sv::render_mode : sv::u8
{
    /// The hybrid where the device has `sg::feature::raytracing_pipeline` and `ray_query` both, and `raster` where it has not, which the viewer warns of once.
    /// A layer left here that names no denoiser is denoised by `automatic`'s, since a hybrid frame is one sample per pixel.
    ///
    /// The hybrid rather than a path tracer because it holds a frame rate: about 8 to 16 ms a moving frame at 1920x1200 where the wavefront takes 15 to 39, on Bistro and Sponza.
    /// It lights one bounce, though, so a room lit only indirectly is darker and noisier than the wavefront converges to; a caller who wants that names `wavefront`.
    /// Resolved where the viewer builds the frame's plan, so the plan declares what the mode it resolved to writes.
    /// A plan built by hand from a layer still `automatic` traces it with the wavefront.
    automatic,

    /// One ray-tracing dispatch per frame, every path traced to its end inside it.
    /// A frame costs `render_settings::samples_per_pixel` samples per pixel.
    megakernel,

    /// Paths split into stages that each run as their own dispatch, and carried across frames.
    /// A still frame costs what its share of `viewer_config::frame_budget_ms` holds, or `rounds_per_frame` rounds without one.
    /// Where one full round overruns that share, it runs one round and starts new samples in only a share of the free slots.
    /// A frame that sees the image change starts one sample per pixel and runs each to its end.
    wavefront,

    /// No paths at all: the same materials drawn by sg raster pipelines, lit directly by the layer's lights and by its environment.
    ///
    /// **A different image, not a cheaper estimate of the same one**: nothing casts a shadow and nothing bounces.
    /// Every frame is final, so the layer counts as converged as soon as one is drawn, and it never denoises.
    /// It writes the primary hit's G-buffer beside the image every frame, which is the seam a hybrid lighting pass grows from.
    /// Quadric batches are not drawn yet.
    /// libs/graphics/shaped-viewer/docs/raster.md is the design.
    raster,

    /// Rasterized primary hits, lit by the tracer's next-event estimate with shadow rays traced inline, and one bounce.
    ///
    /// One light sample and one bounce per pixel and sample, so a still image accumulates, as a traced layer accumulates its paths.
    /// A moving frame is one sample; a still one packs as many as its share of `viewer_definition::still_budget_ms` holds, or `still_samples_per_frame` without one.
    /// Glass spends no bounce: a bounce refracts through it, and what it meets beyond is lit as a primary hit is.
    /// It writes the primary hit's G-buffer every frame, as `raster` does; `render_settings::hybrid_lighting` says where the lighting runs.
    /// It needs `ray_query` and `raytracing_pipeline` both.
    /// libs/graphics/shaped-viewer/docs/plans/hybrid-renderer.md is the design.
    hybrid,
};

/// Where a hybrid frame lights its primary hits; the two are built to be measured against each other.
enum class sv::hybrid_lighting : sv::u8
{
    /// In the material's own pixel stage, through its full closure, tracing as it goes: nothing but the guides is stored.
    forward,

    /// In a compute kernel over the thin G-buffer, through the closure the G-buffer reduces the material to.
    deferred,
};

/// What denoises a still image once the temporal member has handed over, `render_settings::temporal_denoise_frames` after the image last changed.
enum class sv::still_denoise : sv::u8
{
    /// The spatial member `reconstruct` resolves to, on the mean, every frame, crossfaded in over the hand-off.
    spatial,

    /// OIDN on the mean, run again each time the mean's samples double, its last result shown in between.
    /// Reference quality at roughly 0.2 s a megapixel, so each run is a hitch the doubling keeps rare.
    oidn,

    /// The temporal member keeps running, fed the mean rather than each frame's own samples.
    temporal_on_mean,
};

/// A quantity a wavefront layer averages at its pixels' primary hits beside its image: a bit of `render_settings::channels`.
///
/// Each is one float4 a pixel, the mean over the pixel's samples since the restart; a sample whose primary ray escaped adds 0.
/// The values are `channel_*` of shaders/sgl/tracer_wavefront.sgl, which writes them.
enum class sv::render_channel : sv::u32
{
    /// The hit's world position, and 1 in .w, so .w is how much of the pixel a surface covers.
    position = 1,
    /// The hit's depth along the camera's forward axis, in .x.
    view_depth = 2,
    /// The surface's normal, turned toward the ray.
    geometric_normal = 4,
    /// The normal its material shades with, its normal map applied.
    shading_normal = 8,
    /// The material's base color.
    base_color = 16,
    /// The material's specular roughness, base metalness and micro-occlusion, and its opacity in .w.
    material = 32,
    /// The hit instance's index in .x, and a number in [0, 1) of it in .y, the same for every hit on it.
    instance = 64,
};

/// View-wide render settings — everything that applies to a whole view rather than a single scene item.
///
/// How the layer is drawn, traced or rasterized, and the controls of each; a field says which mode reads it.
/// Lighting and the environment live elsewhere on the layer (its lights and its background).
/// This is the home for the post-process sequence, the display transform, ... as they land.
struct sv::render_settings
{
    /// Primary rays integrated per pixel, all in the one path-trace dispatch: the megakernel's alone, which no other mode reads or restarts on.
    ///
    /// This is what one frame costs, not what the image converges to — a view left alone accumulates without limit,
    /// so this trades how fast a moving camera settles against how smooth it stays while it moves.
    i32 samples_per_pixel = 4;

    /// How many jittered samples per pixel a still image of a traced layer is expected to average; 0 for unknown.
    /// That average already filters a pixel, so each primary ray reads its textures through a footprint narrowed by
    /// 1/sqrt of it, held at an eighth, as pbrt-v4 scales its ray differentials; 0 and 1 read a whole pixel's.
    /// A capture that accumulates a known count sets it, and its image comes out as sharp as its samples allow.
    u32 expected_samples_per_pixel = 0;

    /// The channels a wavefront layer averages at its pixels' primary hits beside its image: `sv::render_channel` values OR'd, 0 for none.
    /// `sv::render_settled` hands them back with the image; no other mode writes them.
    u32 channels = 0;

    /// The megakernel's noise target, 0 for none: the standard error a pixel's mean may keep before it stops tracing.
    ///
    /// Measured on the display's scale after a Reinhard curve, so 0.004 is about one step of an 8-bit image, and a bright pixel's noise counts for what it looks like.
    /// Each pixel then keeps its own count and the variance of its frames, so samples go where the image is still noisy: shadows, caustics, glossy reflections.
    /// A layer whose every pixel is down to it counts as at its target, so a capture moves on.
    /// Never restarts accumulation, except turning it on mid-estimate, where no pixel has statistics yet.
    f32 noise_target = 0.0f;

    /// Path length: the primary hit plus this many bounces.
    /// Up to eight refractions through smooth glass spend none, so a hollow glass vessel still leaves the path its bounces for what lies behind.
    i32 max_bounces = 5;

    /// What draws this layer, `automatic`'s hybrid unless named; switching restarts its image.
    render_mode mode = render_mode::automatic;

    /// Where a `hybrid` layer lights its primary hits, which no other mode reads.
    /// Switching restarts its image, since the deferred lighting reduces the material and so converges elsewhere.
    /// Deferred by default: its one lighting kernel serves every material, where forward compiles the lighting into each material's own stage,
    /// and it is the faster of the two with many materials (3.9 against 4.5 ms on Bistro; Sponza's few favor forward).
    sv::hybrid_lighting hybrid_lighting = sv::hybrid_lighting::deferred;

    /// How many bounces a `hybrid` layer lights past its primary hit, 1 or 2; `max_bounces` is the tracers' and not read there.
    /// The second lights the surface the first met as a primary hit is lit, by direct light weighed against a bounce of its own,
    /// which reaches indirect light a room lit through a door or a window otherwise misses.
    i32 hybrid_bounces = 1;

    /// How many rounds a still frame of the wavefront tracer runs, each moving every live path one segment.
    ///
    /// The wavefront's counterpart to `samples_per_pixel`, which it does not read: this is what a still frame costs, and a path still alive after it carries on next frame.
    /// A frame that sees the image change ignores it, and drains: it runs `wavefront_routine::drain_rounds(max_bounces)` rounds.
    /// Those finish every sample it started but a medium walk deeper than that, which a still frame after it finishes.
    /// Under the viewer's `viewer_config::frame_budget_ms`, it is only where the rounds start, until the frame's own timestamps say what one costs.
    i32 rounds_per_frame = 8;

    /// How long a moving image keeps its moving stream after it last changed, in milliseconds.
    ///
    /// Under a `scale` below native a move traces at the upscaler's extent, and a still image restarts at full size.
    /// A frame or two without input in the middle of a move would flip between them, and each switch starts the moving stream's denoiser over, which flickers.
    /// 0 ends a move on the first frame the image holds still.
    f32 motion_hold_ms = 250.0f;

    /// How this layer's image is denoised before its parent composites it; off unless asked for.
    ///
    /// **Never restarts accumulation short of the layer's sample target.**
    /// The mean is radiance only, and everything this selects is written beside it or computed after it, so none of it
    /// reaches the trace hash — turning it on, off or to another member keeps the mean.
    /// The exception is turning it on at the target: a layer there traces nothing, so the guides would never be written, and the mean starts over instead.
    /// While the mean is young a temporal member denoises this frame's own samples, then a spatial one takes over on
    /// the mean — see `temporal_denoise_frames`.
    ///
    /// **Naming a temporal member names the TEMPORAL phase only**, and the spatial phase past the hand-off stays
    /// `automatic`'s choice.
    /// A temporal member cannot run the spatial phase at all: it would be handed the converging mean with no motion
    /// guide, and would report `unsupported`.
    ///
    /// **A `scale` below native upscales only while the image changes.**
    /// A frame that restarts the mean is traced at the smaller extent with one jitter for all its rays, denoised by the
    /// temporal member and upscaled to the view's size; the first frame the image holds still restarts at full size.
    /// That needs a temporal member, and a layer without drawings, whose depth test reads the view-sized primary depth.
    /// `frame_time_ms` is not read yet.
    sr::reconstruct_settings reconstruct = {.denoiser = sr::denoise_method::none};

    /// For how many accumulated frames after a restart a temporal member denoises this frame's own samples, before the
    /// spatial one takes over on the mean.
    ///
    /// A young mean is barely less noisy than one frame, which is where a temporal member's history pays; an old one
    /// has converged past anything that history could add, and only a spatial member keeps it unbiased.
    /// Only a layer whose `reconstruct.denoiser` may run temporally reads it.
    u32 temporal_denoise_frames = 16;

    /// Over how many accumulated frames the hand-off from the temporal member to the spatial one is crossfaded.
    ///
    /// The two produce visibly different images of the same estimate — one carries reprojected history, the other does
    /// not — so switching between them in one frame is a jump in an image that is otherwise only ever getting quieter.
    /// A viewer that never moves shows it once and it reads as a glitch; one being orbited shows it on every stop.
    ///
    /// Both members run on each frame of the fade, so its length is what it costs.
    /// 0 turns the fade off and hands over in one frame.
    u32 temporal_denoise_fade_frames = 8;

    /// What denoises a still image once the temporal member hands over; never restarts accumulation.
    sv::still_denoise still_denoise = sv::still_denoise::spatial;

    /// The exposure and curve the layer's image is shown through, applied where its parent composites it.
    ///
    /// **Never restarts accumulation**: the mean stays radiance, and only its display changes.
    /// The default curve `none` shows the radiance as it is, so a unorm8 target clips and does not encode it.
    /// Drawings and annotations are drawn after it and see none of it.
    sr::display_transform display = {};
};
