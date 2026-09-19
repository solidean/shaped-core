#include "watchdog.hh"

#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/common/utility.hh>
#include <clean-core/error/crash_handler.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/format.hh>
#include <clean-core/string/print.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread.hh>

#include <thread>

using namespace cc::primitive_defines;

namespace
{
cc::atomic<u64> g_heartbeat = {0};

// How often the watchdog looks, and so how late past the budget it may fire.
constexpr double poll_secs = 1.0;
} // namespace

struct nx::impl::run_watchdog::state
{
    cc::mutex<bool> stop = cc::mutex<bool>(false);
    std::condition_variable wake;
    std::thread thread;
};

void nx::impl::watchdog_heartbeat()
{
    g_heartbeat.fetch_add(1, cc::memory_order_relaxed);
}

nx::impl::run_watchdog::run_watchdog(double quiet_secs, void (*report)(double quiet_secs))
{
#if CC_HAS_THREADS
    if (quiet_secs <= 0)
        return;

    _state = cc::make_unique<state>();
    _state->thread = std::thread(
        [s = _state.get(), quiet_secs, report]
        {
            cc::set_current_thread_name("nexus watchdog");

            auto seen = g_heartbeat.load(cc::memory_order_relaxed);
            auto quiet = 0.0;
            auto const poll = cc::min(poll_secs, quiet_secs);
            while (!s->stop.wait_for(s->wake, poll, [](bool const& stop) { return stop; }))
            {
                auto const now = g_heartbeat.load(cc::memory_order_relaxed);
                if (now != seen)
                {
                    seen = now;
                    quiet = 0.0;
                    continue;
                }

                quiet += poll;
                if (quiet < quiet_secs)
                    continue;

                if (report != nullptr)
                    report(quiet);
                else
                {
                    cc::eprintln("\n[nexus watchdog] no test has started or finished for {:g} s; every thread's stack "
                                 "follows",
                                 quiet);
                    cc::report_all_thread_stacks("[nexus watchdog] no test progress");
                }
                quiet = 0.0; // the next report comes after another full budget, not on every poll
            }
        });
#else
    (void)quiet_secs;
    (void)report;
#endif
}

nx::impl::run_watchdog::~run_watchdog()
{
    if (_state == nullptr)
        return;
    _state->stop.lock([](bool& stop) { stop = true; });
    _state->wake.notify_all();
    _state->thread.join();
}
