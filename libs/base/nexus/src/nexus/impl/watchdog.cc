#include "watchdog.hh"

#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/crash_handler.hh>
#include <clean-core/fwd.hh>
#include <clean-core/record/crash_dump.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_backlog.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread.hh>

#include <cstdio>  // stderr, because the report path must stay as small as the crash path it borrows
#include <cstdlib> // std::_Exit: a hung test cannot be unwound, so the run ends here

#if CC_HAS_THREADS && !defined(__EMSCRIPTEN__)
#include <thread>
#endif

using namespace cc::primitive_defines;

namespace
{
cc::atomic<u64> g_heartbeat = {0};

// How often the watching thread looks, and so how late past the budget it may fire.
constexpr double poll_secs = 1.0;
} // namespace

struct nx::impl::run_watchdog::state
{
    double quiet_secs = 0;
    void (*report)(double quiet_secs) = nullptr;

    /// The heartbeat last seen, and when it was last seen to change, on the steady clock.
    struct progress
    {
        u64 seen = 0;
        double changed_secs = 0;
    };
    cc::mutex<progress> last;

#if CC_HAS_THREADS && !defined(__EMSCRIPTEN__)
    cc::mutex<bool> stop = cc::mutex<bool>(false);
    std::condition_variable wake;
    std::thread thread;
#endif
};

void nx::impl::watchdog_heartbeat()
{
    g_heartbeat.fetch_add(1, cc::memory_order_relaxed);
}

void nx::impl::report_hung_run(double quiet_secs)
{
    char buffer[128] = {};
    auto const written
        = cc::format_to(cc::span<char>(buffer), "no test has started or finished for {:.1f} s", quiet_secs);

    std::fputs("\n===================== hung run =====================\n", stderr);
    std::fputs("[nexus watchdog] ", stderr);
    std::fwrite(buffer, 1, size_t(written), stderr);
    std::fputc('\n', stderr);

#if defined(__EMSCRIPTEN__)
    // A spin cannot reach this code at all here, so a report that arrived means the run was WAITING.
    std::fputs("this is a stepped wasm run, so the run was waiting rather than spinning\n", stderr);
#endif

    // What work is still outstanding, which for a hang that is a WAIT is usually the whole answer.
    cc::report_async_backlogs("no test progress");

    // The running tests, every thread's stack, then every thread's open scopes.
    // report_stacks prints all three — the first through nexus's own crash-context hook — and runs the other hooks too,
    // one of which writes a constrained dump to the installed path.
    // An ASYNC_TEST that awaits forever holds no thread, so "running test: <none>" there is the usual shape of this report.
    cc::report_all_thread_stacks("[nexus watchdog] no test progress");

    // **The recording last**, so it replaces the constrained dump the hooks above just wrote.
    // Quiescent, which pauses the consumer first and closes the chunk-recycling race the crash path lives with.
    if (cc::rec::is_crash_dump_installed())
    {
        auto const wrote = cc::rec::write_dump_now(cc::rec::dump_mode::quiescent);
        auto const path = cc::rec::crash_dump_path();

        std::fputs(wrote ? "\nrecording written to " : "\nrecording could NOT be written to ", stderr);
        if (path.empty())
            std::fputs("<the installed sink>", stderr);
        else
            std::fwrite(path.data(), 1, size_t(path.size()), stderr);
        std::fputc('\n', stderr);
    }

    std::fputs("====================================================\n", stderr);
    std::fflush(stderr);

    // _Exit rather than exit: running static destructors would mean joining the threads that are stuck.
    std::_Exit(4);
}

nx::impl::run_watchdog::run_watchdog(double quiet_secs, void (*report)(double quiet_secs))
{
    if (quiet_secs <= 0)
        return;

    _state = cc::make_unique<state>();
    _state->quiet_secs = quiet_secs;
    _state->report = report != nullptr ? report : &report_hung_run;
    _state->last.lock(
        [](state::progress& p)
        {
            p.seen = g_heartbeat.load(cc::memory_order_relaxed);
            p.changed_secs = cc::current_time_steady_secs();
        });

#if CC_HAS_THREADS && !defined(__EMSCRIPTEN__)
    // Not started under wasm even with threads: the run is stepped from the host loop, which calls poll() instead.
    _state->thread = std::thread(
        [this]
        {
            cc::set_current_thread_name("nexus watchdog");

            auto const poll_interval = cc::min(poll_secs, _state->quiet_secs);
            while (!_state->stop.wait_for(_state->wake, poll_interval, [](bool const& stop) { return stop; }))
                poll();
        });
#endif
}

nx::impl::run_watchdog::~run_watchdog()
{
    if (_state == nullptr)
        return;

#if CC_HAS_THREADS && !defined(__EMSCRIPTEN__)
    _state->stop.lock([](bool& stop) { stop = true; });
    _state->wake.notify_all();
    _state->thread.join();
#endif
}

void nx::impl::run_watchdog::poll()
{
    if (_state == nullptr)
        return;

    auto const now = cc::current_time_steady_secs();
    auto const beat = g_heartbeat.load(cc::memory_order_relaxed);

    auto quiet = 0.0;
    auto const due = _state->last.lock(
        [&](state::progress& p)
        {
            if (beat != p.seen)
            {
                p.seen = beat;
                p.changed_secs = now;
                return false;
            }

            quiet = now - p.changed_secs;
            if (quiet < _state->quiet_secs)
                return false;

            // The next report comes after another full budget, not on every poll.
            p.changed_secs = now;
            return true;
        });

    if (due)
        _state->report(quiet);
}
