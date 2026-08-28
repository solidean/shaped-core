#pragma once

#include <shaped-viewer/fwd.hh>

namespace sv
{
/// Where a layer stops accumulating, which bounds GPU LOAD rather than the estimate.
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
/// Per view rather than per process is the intended shape — see libs/graphics/shaped-viewer/docs/TODO.md, where this becomes
/// `render_settings::max_accumulated_frames`, excluded from the trace hash.
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
    ///
    /// This is what one frame costs, not what the image converges to — a view left alone accumulates without limit,
    /// so this trades how fast a moving camera settles against how smooth it stays while it moves.
    i32 samples_per_pixel = 4;

    /// Path length: the primary hit plus this many diffuse bounces.
    i32 max_bounces = 5;
};
