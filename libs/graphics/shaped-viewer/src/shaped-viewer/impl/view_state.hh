#pragma once

#include <clean-core/string/string.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/layout/layout_tree.hh>
#include <shaped-viewer/view/camera.hh>
#include <shaped-viewer/view/camera_controller.hh>
#include <typed-geometry/geometry/primitives/aabb.hh>

namespace sv::impl
{
/// What a view keeps across frames on the CPU side, keyed by its view_id.
///
/// This is the half a caller or the built-in controller reads and writes; the GPU half — the textures and their
/// counters — lives on view_renderer, next to the trace that owns them.
/// The two never exchange a signal: the trace decides for itself whether its image is still valid, by hashing what it
/// uploads, so this state going missing simply restarts the image rather than corrupting it.
struct view_state
{
    /// What this view is called where a human reads it — the id up to its `##`, unless the caller set one.
    ///
    /// Persistent rather than per-frame because a `view_data` is rebuilt every frame, and nothing renders a name yet:
    /// paying a string allocation per view per frame for text no pass reads would be the wrong trade.
    cc::string display_name;

    /// Whether the one-shot seeding setters have already run for this view.
    bool camera_seeded = false;

    orbit_camera_controller controller = {};

    /// The camera this view renders from when the caller does not supply one.
    /// Kept alongside the controller's orbit because a caller may set a camera the orbit cannot express exactly.
    sv::camera camera = {};

    /// Whether `view_ref::camera` was called while authoring this frame, and the previous one.
    ///
    /// Input is routed before authoring, so routing reads the *previous* frame's flag — the last complete answer to
    /// "does the caller drive this view". Rolling these over the wrong way lets the controller fight a caller-driven
    /// camera for one frame every time the answer changes.
    bool camera_owned_this_frame = false;
    bool camera_owned_last_frame = false;

    /// Where this view landed last frame, in window space, taken from the plan's hit region.
    /// Nothing hit-tests against it any more — `pick_hit_region` does that — but a drag reads it to size the pane it lifts.
    tg::aabb2i rect = {};

    /// False until the view has been resolved once; a rect that means nothing yet must not be read.
    bool has_rect = false;

    /// Draw order within the frame, for debugging and for a caller that wants to know what was in front.
    u32 order = 0;

    /// Whether the caller offered this view for dragging, re-asserted every frame like `camera_owned_this_frame`.
    ///
    /// Routing runs before authoring, so a drag starting this frame has to consult the *previous* frame's answer —
    /// which is the last complete one there is.
    /// A view that stops being offered stops being draggable, but keeps wherever it was already put.
    bool movable_this_frame = false;
    bool movable_last_frame = false;

    /// Where a drag put this view, as a fraction of the window.
    ///
    /// Only meaningful once `placement_seeded`: until the first drag the view flows in the layout the caller wrote.
    /// A fraction rather than a pixel rect, so a lifted view keeps its place across a window resize — and so the
    /// caller's tree can be rebuilt from scratch every frame without the viewer having to remember rects.
    bool placement_seeded = false;
    relative_placement placement;

    /// The key-bound zoom into this view's image, and where it is centred in the view's own uv.
    ///
    /// It magnifies what a leaf *samples*, never the camera: the trace is untouched, so zooming inspects the converged
    /// image rather than restarting it.
    /// Paired with `sampler_mode::nearest` that is a pixel-exact readout.
    float zoom = 1.0f;
    tg::pos2f zoom_center = tg::pos2f(0.5f, 0.5f);

    /// What the renderer already holds for this view, which is what a refresh policy is measured against.
    ///
    /// Kept on the CPU half rather than read back out of the renderer, so building a plan stays a pure function of
    /// values a test can supply.
    /// Losing it only costs one extra refresh.
    bool has_target = false;
    tg::vec2i target_resolution = tg::vec2i(0, 0);
    u64 last_refresh_frame = 0;

    /// Whether this view holds the camera drag, so motion keeps reaching it once the cursor leaves its rect.
    bool is_active = false;
};
} // namespace sv::impl
