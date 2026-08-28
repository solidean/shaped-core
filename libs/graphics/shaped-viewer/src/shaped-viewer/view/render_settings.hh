#pragma once

#include <shaped-viewer/fwd.hh>

namespace sv
{
/// Frames a traced layer may accumulate before it stops weighting new samples in.
///
/// The running mean is kept in half floats, so its weight has to stay well inside their precision.
/// A layer sitting here is **converged as far as it will ever get**, which is why the cap is public: a caller waiting
/// for a settled image has to know the difference between "not there yet" and "this is as good as it gets", and a
/// target above the cap would otherwise wait forever.
/// `view_ref::is_accumulation_converged` is the answer that already applies it.
///
/// Per view rather than per process is the intended shape — see libs/graphics/shaped-viewer/docs/TODO.md, where this becomes
/// `render_settings::max_accumulated_frames` and drops out of the trace hash.
inline constexpr u32 accumulation_frame_cap = 4096;
} // namespace sv

/// View-wide render settings — everything that applies to a whole view rather than a single scene item.
///
/// Just the path-tracer integration controls for now.
/// Lighting and the environment live elsewhere on the view (its area_lights list and its background).
/// This is the home for the post-process sequence, exposure, ... as they land.
struct sv::render_settings
{
    /// Primary rays integrated per pixel, all in the one path-trace dispatch.
    /// Higher is cleaner but costlier.
    /// A still camera converges further once temporal accumulation is wired through the view renderer.
    i32 samples_per_pixel = 4;

    /// Path length: the primary hit plus this many diffuse bounces.
    i32 max_bounces = 5;

    /// Renders how much history each pixel kept, instead of the image.
    ///
    /// Red is a pixel that carried nothing this frame — its history was rejected, so it is showing a raw
    /// single-frame estimate and will keep doing so.
    /// Green means it is accumulating.
    /// That distinction is otherwise invisible: a rejection bug and a genuinely high-variance region look identical
    /// in the final image, and only this separates them.
    ///
    /// Toggling it restarts the estimator once, since it changes what the trace writes.
    bool debug_accumulation = false;
};
