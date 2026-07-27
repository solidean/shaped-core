#pragma once

#include <shaped-viewer/fwd.hh>

namespace sv
{
/// View-wide render settings — everything that applies to a whole view rather than a single scene item.
///
/// Just the path-tracer integration controls for now. Lighting and the environment live elsewhere on the view
/// (its area_lights list and its background); this is the home for the post-process sequence, exposure, ... as
/// they land.
struct render_settings
{
    /// Primary rays integrated per pixel, all in the one path-trace dispatch. Higher is cleaner but costlier;
    /// a still camera converges further once temporal accumulation is wired through the view renderer.
    i32 samples_per_pixel = 4;

    /// Path length: the primary hit plus this many diffuse bounces.
    i32 max_bounces = 5;
};
} // namespace sv
