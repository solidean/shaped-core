#pragma once

#include <clean-core/container/map.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/string.hh>
#include <shaped-graphics/resource/texture.hh>
#include <shaped-rendering/reconstruct.hh>
#include <shaped-viewer/fwd.hh>
#include <shaped-viewer/layout/layout_tree.hh>
#include <shaped-viewer/rendering/traced_group.hh>
#include <shaped-viewer/rendering/wavefront_routine.hh> // wavefront_timing, held by value
#include <shaped-viewer/view/camera.hh>
#include <shaped-viewer/view/camera_controller.hh>
#include <typed-geometry/linalg/pos.hh>
#include <typed-geometry/linalg/vec.hh>

namespace sv::impl
{
/// How a still wavefront frame fits its time budget: a smoothed estimate of one round's GPU time, and the frames whose
/// timestamps have not landed yet.
struct wavefront_pacing
{
    /// GPU seconds per round, or 0 before any frame has been measured.
    f64 seconds_per_round = 0.0;

    /// One traced frame, kept until its timestamps land.
    struct pending
    {
        wavefront_timing timing;
        i32 rounds = 0;
    };
    cc::vector<pending> in_flight;
};

/// One temporal resource a view keeps across frames, and how far whatever writes it has run.
///
/// `reset_hash` is the declaration's, carried over from the frame that allocated this: a mismatch against the frame
/// now asking for it means the history describes something else, and `accum_frame` restarts at 0.
/// A resize does the same, since a fresh texture holds nothing to blend into.
struct temporal_slot
{
    /// What the frame writes, and what everything downstream samples.
    ///
    /// One texture rather than a pair: the tracer reads it back at the pixel it is about to write, so its blend is
    /// race-free in place — and a throttled view re-presents exactly the image it last produced.
    sg::texture_2d texture;

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

    /// What the last frame of this accumulation slot's layer bound in place of permutations not compiled, as `stand_in_report::hash`.
    /// The next frame's image hash folds it in, which is what restarts the image when the set changes, and only then.
    u64 stand_in_hash = 0;

    /// The denoiser's own state, held only by a `temporal_id::denoised` slot.
    ///
    /// Here rather than on a routine because a routine cannot know which layer a call belongs to, and here rather than
    /// beside the view because it lives and dies with the image it produces.
    sr::reconstruct_history denoise;

    /// The temporal member's state for the frames traced at the upscaler's smaller extent, held only by a
    /// `temporal_id::frame_samples` slot.
    /// Apart from `denoise` because the two streams run at different extents: one history would be rebuilt on every
    /// switch between moving and still.
    sr::reconstruct_history scaled_denoise;

    /// The camera this slot's layer was last traced from, held only by a `temporal_id::motion_guide` slot.
    /// The next frame's motion vectors reproject into it.
    shaders::tracer::camera_record last_camera = {};
    bool has_last_camera = false;

    /// The wavefront tracer's paths, held only by a `temporal_id::accumulation` slot whose layer it traces.
    ///
    /// Here because `texture` is the mean of exactly these paths' tallies: the two restart together, and one without the other is a mean of nothing.
    /// Unset until the layer's first wavefront frame, and replaced whenever the extent or the permutation count outgrows it.
    cc::optional<wavefront_state> paths;

    /// The wavefront paths of a frame traced at the upscaler's input extent, held beside `paths` for the same reason
    /// `scaled_denoise` is beside `denoise`.
    cc::optional<wavefront_state> scaled_paths;

    /// Whether the last frame of this accumulation slot's layer was traced at the upscaler's input extent.
    /// The next frame is full-size and restarts whenever the image did not change since, so this is what ends a move.
    bool traced_scaled = false;

    /// How many frames were traced at the input extent, which picks each one's jitter and random sequence.
    u32 scaled_frame = 0;

    /// How many full-size frames of this accumulation slot's layer restarted its mean, which keeps each restart's random sequence apart.
    u32 restarts = 0;

    /// The jitter the last of those frames was traced at, which the upscaler reads beside this frame's.
    tg::vec2f last_jitter = tg::vec2f(0, 0);

    /// What a round of those paths costs, as the frames' own timestamps measured it; a frame budget picks rounds from it.
    wavefront_pacing pacing;

    /// Whether this accumulation slot's raster layer has said that it skips the quadric batches it holds, which it says once.
    bool raster_skip_reported = false;

    /// Whether this accumulation slot's traced layer has said that the device cannot trace it, which it says once.
    bool untraceable_reported = false;
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
    /// Persistent rather than per-frame because a `view_data` is rebuilt every frame; a title strip reads it once per
    /// frame, when the leaves' titles are set in glyphs before the plan.
    cc::string display_name;

    /// Whether the one-shot seeding setters have already run for this view.
    bool camera_seeded = false;

    orbit_camera_controller controller = {};

    /// The fly controller, which unlike the orbit one integrates over time — so the viewer updates it every frame rather
    /// than only on frames that saw an event.
    fps_camera_controller fly = {};

    /// Which of the two drives this view, re-asserted every frame like `movable_this_frame`.
    ///
    /// Routing runs before authoring, so it reads the *previous* frame's answer — the last complete one there is.
    /// `style_seeded` is what makes a switch re-seed the incoming controller from the camera the outgoing one left, rather
    /// than snapping to whatever pose it still held from before.
    camera_style style_this_frame = camera_style::orbit;
    camera_style style_last_frame = camera_style::orbit;
    bool style_seeded = false;

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

    /// Whether the caller declared a camera cut that no frame has traced yet.
    ///
    /// Sticky rather than per-frame: a throttled view may be several frames from its next trace, and dropping the
    /// request in between would leave the very history the cut exists to throw away.
    bool camera_cut_pending = false;

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

// -- Pacing a still wavefront frame.

/// The GPU milliseconds the still wavefront layers of a frame under `config` share: none in a headless viewer.
/// A headless run is a capture or a test, whose image must not depend on how fast the machine is.
[[nodiscard]] f32 wavefront_budget_of(viewer_config const& config);

/// One still wavefront layer's share of `budget_ms`, split evenly among `still_layers` of them; 0 for no budget.
[[nodiscard]] f32 wavefront_budget_share(f32 budget_ms, isize still_layers);

/// The most rounds a budget gives a still frame, however cheap a round measured.
inline constexpr i32 max_budgeted_rounds = 64;

/// How many rounds a still frame runs: `rounds_per_frame`, or as many as fit `budget_ms` at `seconds_per_round`, between 1 and `max_budgeted_rounds`.
/// `rounds_per_frame` stands until a round is measured, and always where the budget is 0.
[[nodiscard]] i32 still_rounds_for(i32 rounds_per_frame, f32 budget_ms, f64 seconds_per_round);

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
