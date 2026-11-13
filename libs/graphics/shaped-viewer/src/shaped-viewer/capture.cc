#include <clean-core/common/utility.hh> // cc::move, cc::max, cc::exchange
#include <clean-core/string/format.hh>
#include <shaped-viewer/impl/capture_session.hh>

namespace sv::impl
{
void capture_session::note_registered(cc::string_view name)
{
    for (auto const& n : _registered)
        if (n == name)
            return;

    _registered.push_back(cc::string(name));
}

void capture_session::hold(cc::string_view reason)
{
    if (_hold_reason.empty())
        _hold_reason = reason.empty() ? cc::string("an unnamed hold") : cc::string(reason);
}

cc::string capture_session::take_hold()
{
    return cc::exchange(_hold_reason, cc::string());
}

void capture_session::begin()
{
    _start_seconds = _request.clock_seconds();
    if (_run_start_seconds < 0.0)
        _run_start_seconds = _start_seconds;
    if (has_budget())
    {
        auto const frames_left = _request.is_sequence() ? cc::max(1, _request.frames - _frame) : 1;
        _frame_share = cc::max(0.0, _request.budget_seconds - run_elapsed_seconds()) / double(frames_left);
    }
}

double capture_session::run_elapsed_seconds() const
{
    return _run_start_seconds < 0.0 ? 0.0 : _request.clock_seconds() - _run_start_seconds;
}

void capture_session::advance_frame()
{
    ++_frame;
    _settled_before_writing = false;
    begin();
}

double capture_session::elapsed_seconds() const
{
    return _request.clock_seconds() - _start_seconds;
}

bool capture_session::is_out_of_time() const
{
    // Under a budget a frame's share is the plan, so the failure point sits past it, with room for the frame that finishes it.
    auto const limit = has_budget() ? cc::max(_request.timeout_seconds, _frame_share + budget_grace_seconds)
                                    : _request.timeout_seconds;
    return elapsed_seconds() >= limit;
}

bool capture_session::is_share_spent() const
{
    return has_budget() && elapsed_seconds() + _viewer_frame_seconds >= _frame_share;
}

void capture_session::note_viewer_frame()
{
    auto const now = _request.clock_seconds();
    if (_last_viewer_frame >= 0.0)
    {
        auto const took = now - _last_viewer_frame;
        _viewer_frame_seconds = _viewer_frame_seconds <= 0.0 ? took : 0.8 * _viewer_frame_seconds + 0.2 * took;
    }
    _last_viewer_frame = now;
}

cc::string partial_capture_path(cc::string_view path)
{
    // The extension is what picks the container format, so `.partial` goes before it rather than after: a
    // `capture.jpg.partial` would be written as a JPEG and open as nothing.
    auto const dot = path.rfind('.');
    auto const slash = cc::max(path.rfind('/'), path.rfind('\\'));
    if (dot < 0 || dot < slash)
        return cc::format("{}.partial", path);

    return cc::format("{}.partial{}", path.subview({.offset = 0, .size = dot}), path.subview(dot));
}

bool capture_session::note_owed_work(isize owed)
{
    auto const drained = _owing && owed == 0;
    _owing = owed > 0;
    return drained;
}

bool capture_session::is_settled(bool views_converged, bool any_traced, isize pending_work, bool traces_ran) const
{
    // Post-load work changes a texture's contents rather than its id, so it never restarts a view's accumulation.
    // Without this term an image whose mips are still generating settles at full count and looks subtly wrong.
    if (pending_work > 0)
        return false;

    // A trace that never dispatched still lets its accumulator climb, so convergence alone would accept a black image.
    // Only consulted where there was a trace to run at all.
    if (any_traced && !traces_ran)
        return false;

    return !any_traced || views_converged;
}
} // namespace sv::impl
