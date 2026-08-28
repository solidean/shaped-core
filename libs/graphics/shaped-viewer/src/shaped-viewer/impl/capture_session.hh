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

    /// Starts the clock, once, when the loop opens.
    void begin();

    /// Whether the run has spent its whole timeout.
    [[nodiscard]] bool is_out_of_time() const;

    /// How long the run has taken so far, in seconds.
    [[nodiscard]] double elapsed_seconds() const;

    /// Whether the image is finished: every traced view converged, nothing still owing post-load work, and a trace
    /// that actually dispatched.
    ///
    /// `views_converged` is the caller's fold over every refreshing trace, through
    /// `view_store::is_accumulation_converged` — which is the rule, and which is why this takes an answer rather than
    /// a list of counts.
    /// Comparing counts here is what it used to do, and a layer that stops at `sv::accumulation_frame_cap` made an
    /// accumulate target above the cap unreachable: the run burned its whole timeout on an image that had converged.
    ///
    /// `any_traced` says whether there was a refreshing trace at all.
    /// A frame with none cannot report a dispatch, so `traces_ran` is only consulted when there was something to run —
    /// otherwise a 2D-only view could never settle.
    [[nodiscard]] bool is_settled(bool views_converged, bool any_traced, isize pending_work, bool traces_ran) const;

    /// Whether the image has already been written, so the run is finishing rather than still converging.
    [[nodiscard]] bool is_done() const { return _done; }
    void mark_done() { _done = true; }

    /// Whether the run reached its own bar, as opposed to writing what it had when the clock ran out.
    [[nodiscard]] bool settled_before_writing() const { return _settled_before_writing; }
    void mark_settled_before_writing() { _settled_before_writing = true; }

private:
    sr::capture_request _request;
    cc::vector<cc::string> _registered;

    bool _applied = false;
    bool _done = false;
    bool _settled_before_writing = false;

    /// Steady-clock ticks, kept as a double of seconds so this header pulls in no <chrono>.
    double _start_seconds = 0.0;
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
