#pragma once

#include <clean-core/container/vector.hh>
#include <shaped-viewer/background.hh>
#include <shaped-viewer/camera.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/light.hh>
#include <shaped-viewer/render_settings.hh>
#include <shaped-viewer/scene_item.hh>
#include <shaped-viewer/view_id.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sv
{
/// One thing to render this frame: a camera looking at a list of scene items, lit by the view's lights and environment, traced into a single target texture of `size` pixels.
///
/// A view is a plain per-frame value — build a fresh one each frame.
/// Its `id` is the only part that persists: it names the accumulators and history the view reuses across frames.
/// The renderer hands back one target texture per view, which a caller then blits into an output window (or a region of one).
///
/// Lights are held in typed lists (one per kind) rather than mixed into `items`, so a consumer takes exactly the light type it handles.
/// The `background` is the environment a missed ray sees.
struct view
{
    /// stable identity naming the accumulators and history reused across frames
    view_id id;

    /// target texture size in pixels
    tg::vec2i size = tg::vec2i(1280, 720);

    /// the eye this view is traced from
    sv::camera camera;

    /// environment a missed ray sees
    sv::background background;

    /// geometry to render, each with its own material
    cc::vector<scene_item> items;

    /// area lights illuminating the scene
    cc::vector<area_light> area_lights;

    /// per-view renderer knobs (sample counts, bounce limits, …)
    render_settings settings;
};
} // namespace sv
