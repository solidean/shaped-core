#include <clean-core/common/utility.hh> // cc::move
#include <shaped-viewer/impl/capture_session.hh>

#include <chrono>

namespace sv::impl
{
namespace
{
[[nodiscard]] double now_seconds()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void capture_session::note_registered(cc::string_view name)
{
    for (auto const& n : _registered)
        if (n == name)
            return;

    _registered.push_back(cc::string(name));
}

void capture_session::begin()
{
    _start_seconds = now_seconds();
}

double capture_session::elapsed_seconds() const
{
    return now_seconds() - _start_seconds;
}

bool capture_session::is_out_of_time() const
{
    return elapsed_seconds() >= _request.timeout_seconds;
}

bool capture_session::is_settled(cc::span<u32 const> traced_views, isize pending_work, bool traces_ran) const
{
    // Post-load work changes a texture's contents rather than its id, so it never restarts a view's accumulation.
    // Without this term an image whose mips are still generating settles at full count and looks subtly wrong.
    if (pending_work > 0)
        return false;

    // A trace that never dispatched still lets its accumulator climb, so the frame count alone would accept a black image.
    // Only consulted where there was a trace to run at all.
    if (!traced_views.empty() && !traces_ran)
        return false;

    for (auto const frames : traced_views)
        if (frames < _request.accumulate_frames)
            return false;

    return true;
}
} // namespace sv::impl
