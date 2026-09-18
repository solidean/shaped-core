#include <clean-core/record/scope.hh>
#include <clean-core/thread/atomic.hh>
#include <nexus/impl/hang_watchdog.hh>
#include <nexus/test.hh>

// The hang watchdog's policy, tested by naming the two times rather than by waiting one out.
//
// **A timeout is tested by injecting a clock, never by sleeping** — libs/base/nexus/docs/test-runtime.md.
// Which is why evaluate_hang takes `now` instead of reading it: the boundary is the thing worth pinning, and a test
// that slept through a real deadline would take as long as the deadline and still only prove one side of it.
//
// The thread and the host-loop tick around it are plumbing: a condition variable and a timer, with no decision in
// either.

using nx::impl::evaluate_hang;
using nx::impl::hang_verdict;
using nx::impl::hang_watchdog_config;
using nx::impl::running_test_snapshot;

namespace
{
[[nodiscard]] running_test_snapshot started_at(double secs, cc::string_view name = "a-test")
{
    return running_test_snapshot{.name = name, .section = 0, .started_secs = secs};
}
} // namespace

TEST("nexus/hang - nothing overdue is the answer while every deadline is unreached")
{
    auto const config = hang_watchdog_config{.per_test_secs = 10, .per_run_secs = 100};
    running_test_snapshot const running[] = {started_at(5)};

    // Nine seconds into a ten-second test, ten into a hundred-second run.
    CHECK(evaluate_hang(config, running, 5, 14) == hang_verdict::running);
}

TEST("nexus/hang - the deadline is exclusive, so reaching it exactly is not yet overdue")
{
    auto const config = hang_watchdog_config{.per_test_secs = 10};
    running_test_snapshot const running[] = {started_at(100)};

    // A guard that fires AT its budget would report a test that finished on time.
    CHECK(evaluate_hang(config, running, 0, 110) == hang_verdict::running);
    CHECK(evaluate_hang(config, running, 0, 110.001) == hang_verdict::test_overdue);
}

TEST("nexus/hang - a test is blamed before the run, since it is the more specific answer")
{
    // Both deadlines are blown at once, which is the usual shape: the run is late BECAUSE the test is stuck.
    auto const config = hang_watchdog_config{.per_test_secs = 10, .per_run_secs = 20};
    running_test_snapshot const running[] = {started_at(50)};

    CHECK(evaluate_hang(config, running, 1, 100) == hang_verdict::test_overdue);
}

TEST("nexus/hang - a run with nothing running can still be overdue")
{
    // The case a per-test deadline cannot catch: progress forever, no single test overrunning.
    auto const config = hang_watchdog_config{.per_test_secs = 10, .per_run_secs = 20};

    // A real start time, because zero means "not started yet" here — which the last test in this file pins.
    CHECK(evaluate_hang(config, {}, 1, 22) == hang_verdict::run_overdue);
    CHECK(evaluate_hang(config, {}, 1, 20) == hang_verdict::running);
}

TEST("nexus/hang - the slowest test decides, not the first one looked at")
{
    auto const config = hang_watchdog_config{.per_test_secs = 10};
    running_test_snapshot const running[] = {started_at(95, "fast"), started_at(50, "stuck"), started_at(99, "fast-2")};

    CHECK(evaluate_hang(config, running, 0, 100) == hang_verdict::test_overdue);
}

TEST("nexus/hang - a slot with no start time yet is never overdue")
{
    // Publishing a test is two stores, and a watchdog may look between them.
    // Treating the gap as an infinitely old test would report a hang on every run.
    auto const config = hang_watchdog_config{.per_test_secs = 10};
    running_test_snapshot const running[] = {started_at(0, "just-published")};

    CHECK(evaluate_hang(config, running, 0, 1'000'000) == hang_verdict::running);
}

TEST("nexus/hang - a zero deadline is off rather than instantly overdue")
{
    running_test_snapshot const running[] = {started_at(1)};

    CHECK(evaluate_hang({}, running, 1, 1'000'000) == hang_verdict::running);
    CHECK(evaluate_hang({.per_test_secs = 0, .per_run_secs = 10}, running, 1, 1'000'000) == hang_verdict::run_overdue);
    CHECK(evaluate_hang({.per_test_secs = 10, .per_run_secs = 0}, running, 1, 1'000'000) == hang_verdict::test_overdue);
}

TEST("nexus/hang - a run whose start is unknown is not judged against a run deadline")
{
    // Zero means "not started yet" here for the same reason it does for a test.
    CHECK(evaluate_hang({.per_run_secs = 10}, {}, 0, 1'000'000) == hang_verdict::running);
}

// The end-to-end path, which no automatic run may take: it never returns.
//
// `nx::config::manual` keeps it out of every sweep, so it runs only when named exactly.
// Drive it by hand with a short deadline to see the report the watchdog actually prints:
//
//     uv run dev.py test "nexus/hang - hangs forever" --hang-timeout 2
//
// It is a test rather than a doc paragraph because the report is the deliverable, and the only way to check that
// what it prints is worth reading is to read it.
TEST("nexus/hang - hangs forever", nx::config::manual)
{
    CC_RECORD_SCOPE("a-scope-the-report-should-name");

    // A wait rather than a spin, so this is the shape a stepped wasm run can catch too.
    auto blocked = cc::atomic<bool>(false);
    while (!blocked.load(cc::memory_order_relaxed))
    {
    }

    CHECK(false); // never reached
}
