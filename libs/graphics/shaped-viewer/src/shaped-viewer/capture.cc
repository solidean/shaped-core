#include <clean-core/common/utility.hh> // cc::move, cc::max
#include <clean-core/string/format.hh>
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
