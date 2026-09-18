#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/thread/mutex.hh>
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
