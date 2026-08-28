#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/layout/layout_tree.hh>
#include <shaped-viewer/view/camera.hh>
#include <shaped-viewer/view/camera_controller.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sv::impl
{
/// One temporal resource a view keeps across frames, and how far whatever writes it has run.
///
/// `reset_hash` is the declaration's, carried over from the frame that allocated this: a mismatch against the frame
/// now asking for it means the history describes something else, and `accum_frame` restarts at 0.
/// A resize does the same, since a fresh texture holds nothing to blend into.
struct temporal_slot
{
    /// What the frame writes, and what everything downstream samples.
    sg::texture_2d texture;

    /// The previous *recorded* frame's `texture`, read-only for this frame.
    ///
    /// A pair rather than one texture because reprojection reads at a different pixel than it writes, which in place
    /// would be a race — and the two rotate only on frames that record, so a throttled view keeps re-presenting the
    /// last image it actually produced rather than the one before it.
    sg::texture_2d history;

    /// Whether `history` holds a frame at all, as opposed to an allocated texture nothing has written yet.
    bool has_history = false;

    /// The reset rule of whatever *writes* this resource, as of the last frame it did.
    /// For a traced layer that is the tracer's own content hash, covering the bytes it uploads.
    u64 reset_hash = 0;

    /// The reset rule the *declaration* carried, as of the last frame that resolved this slot.
    ///
    /// Deliberately not the same field as `reset_hash`, though both are "what invalidates this".
    /// They answer for different parties and are written at different points in the frame, so sharing one field
    /// makes each frame's resolve clobber the writer's rule — and an accumulator whose rule is clobbered restarts
    /// every single frame, which looks exactly like the temporal reuse never having been wired up.
    u64 declared_hash = 0;

    u32 accum_frame = 0;
};

/// Everything a view keeps across frames, keyed by its view_id — held by `sv::view_store`.
///
/// One record rather than two: the camera a caller drives and the accumulator the trace blends into are reclaimed on
/// one clock and against one budget, so a view cannot survive as an identity while its texture is released out from
/// under the parent that samples it.
///
/// Nothing here signals the trace.
/// It decides for itself whether its image is still valid, by hashing the bytes it uploads, so a field going stale
/// restarts the image rather than corrupting it.
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

    /// This view's composited image — what its parent samples, and what it re-presents on a throttled frame.
    ///
    /// Null for the frame's output, whose texture is the caller's, and null before the first `view_renderer::resolve`
    /// that sizes it.
    /// Its presence and extent are what a refresh policy is measured against, so nothing mirrors them: `view_history`
    /// is read off this texture rather than stamped alongside it.
    sg::texture_2d composite;

    /// What the view keeps across frames beyond its composite, keyed by `temporal_input::id`.
    ///
    /// Keyed rather than indexed because a temporal resource names itself: a traced layer's accumulator, and whatever
    /// else a view declares, have to survive a layer being inserted above them.
    cc::map<u64, temporal_slot> temporal;

    /// The camera the last recorded trace of this view drew from, and whether there was one.
    ///
    /// Reprojection maps this frame's pixels back through it to find where they were, so this is what makes a camera
    /// move reuse the converged image instead of discarding it.
    /// Per view rather than per temporal slot: every layer of a view is traced from the one camera.
    camera_gpu last_traced_camera = {};
    bool has_last_traced_camera = false;

    /// The frame this view last re-recorded, which is what its refresh rate is measured against.
    /// The one piece of history with no texture to read it off, so the frame stamps it.
    u64 last_refresh_frame = 0;
};

// -- Folding over a view's traced layers.
//
// One rule in one place, because the alternative already went wrong: `view_ref::accumulated_frames` read
// `temporal_id::accumulation(0)` by hand while the capture path read the layer its plan named, so a view whose trace
// was not layer 0 reported zero forever and nothing waiting on it ever finished.
// Every caller asking "how far has this view got" goes through these two.

/// The lowest accumulated-frame count across every traced layer of `state`, and 0 when it has none.
///
/// The lowest rather than the first: a view converges when its slowest layer does.
[[nodiscard]] u32 min_accumulated_frames(view_state const& state);

/// Whether every traced layer of `state` has reached `frames`, or has stopped climbing and never will.
///
/// An unset `frames` asks only the second question — has this view finished as far as it can.
/// The cap matters because it is reachable: a caller asking for more frames than `sv::accumulation_frame_cap` would
/// otherwise wait on a counter that stopped moving, which is a capture burning its whole timeout on an image that
/// was already as converged as it gets.
/// False for a view with no traced layer at all, since nothing there has converged.
[[nodiscard]] bool is_accumulation_converged(view_state const& state, cc::optional<u32> frames);
} // namespace sv::impl
