#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/record/scope.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/async-test.hh>
#include <nexus/impl/watchdog.hh>
#include <nexus/test.hh>

// The heartbeat is process-wide, so these run alone: a sibling test starting or finishing would reset the watchdog.

namespace
{
cc::mutex<int> g_reports = cc::mutex<int>(0);
std::condition_variable g_reported;

void count_report(double)
{
    g_reports.lock([](int& n) { ++n; });
    g_reported.notify_all();
}
} // namespace

TEST("watchdog - reports a run that stops making progress, and keeps reporting", exclusive())
{
#if CC_HAS_THREADS
    g_reports.lock([](int& n) { n = 0; });
    auto const watchdog = nx::impl::run_watchdog(0.01, &count_report);

    // Waits on the reports, never on the clock; the bound only keeps a broken watchdog from hanging the run.
    CHECK(g_reports.wait_for(g_reported, 60.0, [](int const& n) { return n >= 2; }));
#else
    SUCCEED("no second thread to watch from, so a build without threads has no watchdog");
#endif
}

TEST("watchdog - a zero budget watches nothing", exclusive())
{
    // No thread is started, so the destructor has nothing to join: this returning at all is the check.
    auto const watchdog = nx::impl::run_watchdog(0.0, &count_report);
    SUCCEED();
}

// The end-to-end paths, which no automatic run may take: neither returns, and the report ends the process.
//
// `nx::config::manual` keeps them out of every sweep, so they run only when named exactly.
// Drive one by hand with a short budget to read the report the watchdog actually prints:
//
//     uv run dev.py test "watchdog - a waiting test is reported" --watchdog 2
//
// They are tests rather than a doc paragraph because the report is the deliverable, and the only way to check that
// what it prints is worth reading is to read it.

TEST("watchdog - a waiting test is reported", nx::config::manual)
{
    CC_RECORD_SCOPE("a-scope-the-report-should-name");

    // A thread that waits and never comes back, which is native-only: a stepped wasm run never regains control.
    // A sleep rather than an empty spin, since an infinite loop with no side effects is undefined behaviour.
    for (;;)
        cc::this_thread_sleep_secs(0.01);
}

ASYNC_TEST("watchdog - an awaiting async test is reported", nx::config::manual)
{
    // Holds no thread while it waits, so no running-test slot names it; only the missing heartbeat does.
    // This is the shape a stepped wasm run catches as well.
    auto const never = cc::make_async_manual<int>();
    co_await never;
}
