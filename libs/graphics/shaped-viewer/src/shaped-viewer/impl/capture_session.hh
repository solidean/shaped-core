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
    explicit capture_session(capture_request req) : _request(cc::move(req)) {}

    [[nodiscard]] capture_request const& request() const { return _request; }

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

    /// Whether every traced view has settled: enough accumulated frames, nothing still owing post-load work, and a
    /// trace that actually dispatched.
    ///
    /// `traced_views` is one accumulated-frame count per refreshing trace in the plan.
    /// A frame with no traced view at all cannot report a dispatch, so `traces_ran` is only consulted when there was
    /// something to run — otherwise a 2D-only view could never settle.
    [[nodiscard]] bool is_settled(cc::span<u32 const> traced_views, isize pending_work, bool traces_ran) const;

    /// Whether the image has already been written, so the run is finishing rather than still converging.
    [[nodiscard]] bool is_done() const { return _done; }
    void mark_done() { _done = true; }

    /// Whether the run reached its own bar, as opposed to writing what it had when the clock ran out.
    [[nodiscard]] bool settled_before_writing() const { return _settled_before_writing; }
    void mark_settled_before_writing() { _settled_before_writing = true; }

private:
    capture_request _request;
    cc::vector<cc::string> _registered;

    bool _applied = false;
    bool _done = false;
    bool _settled_before_writing = false;

    /// Steady-clock ticks, kept as a double of seconds so this header pulls in no <chrono>.
    double _start_seconds = 0.0;
};

namespace sv::impl
{
/// Reads `texture` back, converts it to RGB and writes it to `path`, picking the format from the extension.
///
/// Blocking: it waits on the download, which is what a capture wants — the run is over either way.
/// The texture must carry `copy_src` usage and be `bgra8_unorm`, which is what a headless viewer's target is.
[[nodiscard]] cc::result<cc::unit> write_capture_image(sg::context& ctx,
                                                       sg::texture_2d const& texture,
                                                       tg::vec2i size,
                                                       cc::string_view path);
} // namespace sv::impl
