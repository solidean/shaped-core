#include "hang_watchdog.hh"

#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/common/time.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/crash_handler.hh>
#include <clean-core/record/crash_dump.hh>
#include <clean-core/record/thread_scopes.hh>
#include <clean-core/string/format.hh>
#include <clean-core/thread/async_backlog.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/thread.hh>
#include <nexus/tests/execute.hh>

#include <cstdio>  // stderr, because the report path must stay as small as the crash path it borrows
#include <cstdlib> // std::_Exit: a hung test cannot be unwound, so the run ends here

#if CC_HAS_THREADS
#include <thread> // the watchdog runs OUTSIDE the run, which is the whole reason it can notice a spin
#endif

using namespace cc::primitive_defines;

namespace
{
/// The most running tests one report names.
/// A fixed array because this runs where allocating is a bad idea and the number is small by construction.
constexpr isize max_reported_tests = 64;

nx::impl::hang_watchdog_config g_config;
cc::atomic<bool> g_armed = false;
cc::atomic<bool> g_fired = false;
double g_run_started_secs = 0;

#if CC_HAS_THREADS
std::thread g_thread;
cc::atomic<bool> g_stop = false;

/// How finely the watchdog checks for its own stop signal.
///
/// Separate from the poll interval, and much shorter: stopping waits for at most one of these, so a coarse value
/// would add a visible pause to the end of every run for a check nobody is waiting on.
constexpr double stop_poll_secs = 0.02;
#endif

void write_secs(double secs)
{
    char buffer[64] = {};
    auto const written = cc::format_to(cc::span<char>(buffer), "{:.1f}s", secs);
    std::fwrite(buffer, 1, size_t(written), stderr);
}

/// Everything a blown deadline has to say, then the exit.
///
/// **Ordered cheapest-first and most-specific-first**, because each step is likelier to fail than the one before it:
/// the test names are plain globals, the scope stacks need the recorder's registry, and the machine stacks need to
/// suspend threads.
/// A report that dies halfway has still said the most useful part.
[[noreturn]] void report_and_exit(char const* what, cc::span<nx::impl::running_test_snapshot const> running, double now)
{
    std::fputs("\n===================== hung run =====================\n", stderr);
    std::fputs("reason: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);

    if (running.empty())
    {
        std::fputs("no test was running, so the run itself made no progress\n", stderr);
    }
    else
    {
        for (auto const& t : running)
        {
            std::fputs("  test \"", stderr);
            std::fwrite(t.name.data(), 1, size_t(t.name.size()), stderr);
            std::fputs("\" running for ", stderr);
            write_secs(now - t.started_secs);
            std::fputc('\n', stderr);
        }
    }

#if defined(__EMSCRIPTEN__)
    // Said plainly rather than left to be discovered.
    // A spin cannot reach this code at all here, so a report that arrived means the run was WAITING — and that is
    // information about what to look for, not a disclaimer.
    std::fputs("\nthis is a stepped wasm run, so the run was waiting rather than spinning\n", stderr);
#endif

    // The scope stacks first: they work on every platform and name the logical task rather than the instruction.
    cc::rec::report_thread_scopes(what);

    // What work is still outstanding, which for a hang that is a WAIT is usually the whole answer.
    // Covers tracked work only, and says so.
    cc::report_async_backlogs(what);

    // Then the recording itself, which is the part a reader opens afterwards rather than reads here.
    //
    // **Quiescent, not constrained.** A hang is not a fault: the process is healthy in every respect except that one
    // thread is not moving, so the dump may stop the consumer first and close the chunk-recycling race the crash
    // path has to live with.
    // The path is printed rather than assumed, since a report nobody can find the file for is a report without it.
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

    // Then the machine stacks, which reach other threads only on Windows and not at all here.
    cc::report_all_thread_stacks(what);

    std::fputs("====================================================\n", stderr);
    std::fflush(stderr);

    // A hung test cannot be unwound and no later result would be trustworthy, so the run ends non-zero.
    // _Exit rather than exit: running static destructors would mean joining the threads that are stuck.
    std::_Exit(4);
}
} // namespace

nx::impl::hang_verdict nx::impl::evaluate_hang(nx::impl::hang_watchdog_config const& config,
                                               cc::span<nx::impl::running_test_snapshot const> running,
                                               double run_started_secs,
                                               double now)
{
    // A test first, because it is the more specific answer: a run overdue BECAUSE one test is stuck should name
    // the test rather than the run.
    if (config.per_test_secs > 0)
        for (auto const& t : running)
            if (t.started_secs > 0 && now - t.started_secs > config.per_test_secs)
                return nx::impl::hang_verdict::test_overdue;

    if (config.per_run_secs > 0 && run_started_secs > 0 && now - run_started_secs > config.per_run_secs)
        return nx::impl::hang_verdict::run_overdue;

    return nx::impl::hang_verdict::running;
}

void nx::impl::check_hang_deadlines() noexcept
{
    if (!g_armed.load(cc::memory_order_acquire))
        return;

    // One report, whatever fires first and however many threads notice at once.
    if (g_fired.load(cc::memory_order_relaxed))
        return;

    auto const now = cc::current_time_steady_secs();

    nx::impl::running_test_snapshot snapshots[max_reported_tests];
    auto const count = nx::impl::snapshot_running_tests(cc::span<nx::impl::running_test_snapshot>(snapshots));
    auto const running = cc::span<nx::impl::running_test_snapshot const>(snapshots, count);

    auto const verdict = nx::impl::evaluate_hang(g_config, running, g_run_started_secs, now);
    if (verdict == nx::impl::hang_verdict::running)
        return;

    if (g_fired.exchange(true, cc::memory_order_relaxed))
        return;

    report_and_exit(verdict == nx::impl::hang_verdict::test_overdue ? "a test exceeded its deadline"
                                                                    : "the run exceeded its deadline",
                    running, now);
}

void nx::impl::start_hang_watchdog(nx::impl::hang_watchdog_config const& config)
{
    nx::impl::stop_hang_watchdog();

    if (config.per_test_secs <= 0 && config.per_run_secs <= 0)
        return;

    g_config = config;
    g_run_started_secs = cc::current_time_steady_secs();
    g_fired.store(false, cc::memory_order_relaxed);
    g_armed.store(true, cc::memory_order_release);

#if CC_HAS_THREADS && !defined(__EMSCRIPTEN__)
    // A thread OUTSIDE the run, which is what makes a spin catchable: it is not waiting on anything the run holds.
    // Not started under wasm even with threads, where the run is stepped from the host loop and
    // check_hang_deadlines is called from there instead.
    g_stop.store(false, cc::memory_order_release);

    g_thread = std::thread(
        []
        {
            auto const interval = g_config.poll_interval_secs > 0 ? g_config.poll_interval_secs : 0.25;
            auto waited = 0.0;

            while (!g_stop.load(cc::memory_order_acquire))
            {
                // Slept in slices rather than for the whole interval, so stopping is prompt while checking stays rare.
                // A sleep long enough to be the wait is also long enough to be felt at every run's end.
                cc::this_thread_sleep_secs(stop_poll_secs);
                waited += stop_poll_secs;
                if (waited < interval)
                    continue;

                waited = 0.0;
                nx::impl::check_hang_deadlines(); // may not return
            }
        });
#endif
}

void nx::impl::stop_hang_watchdog()
{
    g_armed.store(false, cc::memory_order_release);

#if CC_HAS_THREADS && !defined(__EMSCRIPTEN__)
    if (!g_thread.joinable())
        return;

    g_stop.store(true, cc::memory_order_release);
    g_thread.join();
#endif
}
