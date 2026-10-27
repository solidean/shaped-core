#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-viewer/capture.hh>
#include <shaped-viewer/fwd.hh>
#include <typed-geometry/linalg/vec.hh>

/// The live state of one capture run — internal, owned by the viewer, installed only by `sv::interactive`.
///
/// It answers two questions: whether a registered capture is the one being taken, and whether the image is finished.
/// Everything it needs to decide the second is already kept by the viewer, so this holds counters and a clock and no
/// rendering state of its own.
class sv::impl::capture_session
{
public:
    explicit capture_session(sr::capture_request req) : _request(cc::move(req)) {}

    [[nodiscard]] sr::capture_request const& request() const { return _request; }

    /// Records that `name` was registered this run, for the listing mode.
    /// Registration happens every frame, so this is idempotent by name rather than append-only.
    void note_registered(cc::string_view name);

    [[nodiscard]] cc::span<cc::string const> registered_names() const { return _registered; }

    /// Whether `name` is the capture being taken, and so whether its callback should run at all.
    [[nodiscard]] bool is_active(cc::string_view name) const { return _request.name == name; }

    /// Whether the active capture's callback has yet to run, which is what `capture_context::first_frame` reports.
    [[nodiscard]] bool is_first_application() const { return !_applied; }
    void mark_applied() { _applied = true; }

    /// Holds the capture for the frame being authored, `reason` naming what it waits for; the first reason a frame gives wins.
    void hold(cc::string_view reason);

    /// The reason this frame was held for, empty when it was not, and clears it for the next frame.
    [[nodiscard]] cc::string take_hold();

    /// Starts the clock, once, when the loop opens.
    void begin();

    /// Whether the run has spent its whole timeout.
    [[nodiscard]] bool is_out_of_time() const;

    /// How long the run has taken so far, in seconds.
    [[nodiscard]] double elapsed_seconds() const;

    /// Whether the image is finished: every traced view converged, nothing still streaming in or owing post-load work,
    /// and a trace that actually dispatched.
    ///
    /// `views_converged` is the caller's fold over every refreshing trace, through
    /// `view_store::is_accumulation_converged` — which is the rule, and which is why this takes an answer rather than
    /// a list of counts.
    /// Comparing counts here is what it used to do, and a layer that stops at `sv::accumulation_sample_cap` made an
    /// accumulate target above the cap unreachable: the run burned its whole timeout on an image that had converged.
    ///
    /// `any_traced` says whether there was a refreshing trace at all.
    /// A frame with none cannot report a dispatch, so `traces_ran` is only consulted when there was something to run —
    /// otherwise a 2D-only view could never settle.
    [[nodiscard]] bool is_settled(bool views_converged, bool any_traced, isize pending_work, bool traces_ran) const;

    /// Whether work owed on earlier frames has drained by this one, which owes `owed`, so every traced view must start its mean over.
    ///
    /// Owed work such as a BC7 encode or a mip chain changes a texture's contents and keeps its id, so it restarts no trace.
    /// Pixels that reached the target before it landed would hold samples of the old contents: all of them on a fast GPU, a mix on a slow one.
    /// Starting over once it drains makes the image a function of the final resources alone.
    /// True once per drain, so work that comes and goes restarts the views each time it goes, and the timeout still ends the run.
    [[nodiscard]] bool note_owed_work(isize owed);

    /// Whether the image has already been written, so the run is finishing rather than still converging.
    [[nodiscard]] bool is_done() const { return _done; }
    void mark_done() { _done = true; }

    /// Whether the run reached its own bar, as opposed to writing what it had when the clock ran out.
    [[nodiscard]] bool settled_before_writing() const { return _settled_before_writing; }
    void mark_settled_before_writing() { _settled_before_writing = true; }

    /// The sequence frame being rendered, 0 for a still.
    [[nodiscard]] i32 frame_index() const { return _frame; }

    /// The clock every frame body sees during this capture: the request's instant for the frame being rendered.
    /// It holds still while a frame converges, so a body animating off `frame::seconds` changes nothing until it is written.
    [[nodiscard]] double seconds() const { return _request.seconds_at(_frame); }

    /// Whether the frame being rendered is the last one the run writes.
    [[nodiscard]] bool is_last_frame() const { return !_request.is_sequence() || _frame + 1 >= _request.frames; }

    /// Whether the frame being rendered is written at all, or only rendered for the history of the ones that are.
    [[nodiscard]] bool writes_frame() const { return !_request.is_sequence() || _frame % _request.stride == 0; }

    /// Moves a sequence on to its next frame, with a timeout of its own.
    void advance_frame();

    /// How many written frames of a sequence ran out of time before they settled.
    [[nodiscard]] i32 unsettled_frames() const { return _unsettled_frames; }
    void note_unsettled_frame() { ++_unsettled_frames; }

private:
    sr::capture_request _request;
    cc::vector<cc::string> _registered;

    bool _applied = false;
    bool _done = false;
    bool _settled_before_writing = false;

    /// Whether the last frame `note_owed_work` saw owed any work.
    bool _owing = false;

    /// What the frame being authored is held for; empty when nothing holds it.
    cc::string _hold_reason;

    /// The request's `clock_seconds` as the loop opened, or as the current sequence frame began.
    double _start_seconds = 0.0;

    i32 _frame = 0;
    i32 _unsettled_frames = 0;
};

namespace sv::impl
{
/// Where an unsettled capture's image goes: `<out>.partial.<ext>`, or `<out>.partial` when it has no extension.
///
/// A run that spent its whole timeout must leave NOTHING at the requested path.
/// dev.py reads a file there as the capture having succeeded, and a sweep would then refresh a half-converged image
/// over the committed reference and report it as captured.
/// The image is still written, and written beside it, because looking at what the run managed is how the timeout
/// gets fixed.
[[nodiscard]] cc::string partial_capture_path(cc::string_view path);
} // namespace sv::impl
