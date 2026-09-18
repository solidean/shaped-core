#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <nexus/tests/execute.hh>

// Turning a hung run into a report.
//
// A crash announces itself — a signal is raised, a handler runs, and the faulting thread's stack is still there.
// **A hang announces nothing**: nothing faults, nothing is raised, and the only thing that can notice is a clock
// somebody arranged to look at.
// So this is two pieces, and only the first is platform-specific: the thing that notices, and the report it produces.
//
// **What it catches differs by platform, and the difference is not a bug to fix.**
//
// Natively the watchdog is a thread outside the run, so it notices a test that spins as readily as one that waits.
//
// Under wasm the run is stepped from the host's event loop, serial whatever `--jobs` says.
// A test body that does not return never gives the loop back, so no timer, no message and no interrupt can fire —
// the thing that would notice is queued behind the thing it would notice.
// A hang that is a WAIT returns to the loop on every turn and is caught cleanly, which covers every hang the
// never-block discipline in sg and clean-net is built to produce; a spin in straight-line C++ is not covered, and
// the report says so rather than pretending.
// The backstop for that case is outside the module entirely, in the runner that launched it.
//
// **Reaching a deadline fails the run.** A hung test cannot be unwound, so the report is followed by an exit: there
// is no state in which carrying on would produce a trustworthy result.

namespace nx::impl
{
struct hang_watchdog_config;

/// Starts watching, if any deadline is set.
///
/// Idempotent in the sense that a second call replaces the first.
/// Does nothing where there is no thread to watch from AND no host loop to be stepped by, which is no platform
/// this repo supports but is a configuration (`SC_THREADS=OFF` natively) that must not break.
void start_hang_watchdog(hang_watchdog_config const& config);

/// Stops watching and joins, so nothing fires after a run has finished.
void stop_hang_watchdog();

/// What a deadline check concluded.
enum class hang_verdict
{
    running,      ///< nothing is overdue
    test_overdue, ///< some test has been running longer than per_test_secs
    run_overdue,  ///< the run as a whole has been going longer than per_run_secs
};

/// The whole policy, as a function of its inputs and nothing else.
///
/// **Separated from the clock and the thread on purpose.** A timeout tested by waiting one out is a slow test that
/// proves nothing about the boundary; this way the boundary is tested by naming the two times.
/// `started_secs` of zero means a slot that has not published one yet, which is a moment rather than a state and is
/// never overdue.
[[nodiscard]] hang_verdict evaluate_hang(hang_watchdog_config const& config,
                                         cc::span<running_test_snapshot const> running,
                                         double run_started_secs,
                                         double now);

/// Checks the deadlines now, on the calling thread.
///
/// **This is the wasm half.** The host loop calls it between steps, which is the only moment anything gets to run
/// there; natively the watchdog thread calls it and nobody else needs to.
/// Cheap enough to call every turn: a few atomic loads per running test and one clock reading.
void check_hang_deadlines() noexcept;
} // namespace nx::impl

/// What a run is allowed to take before it is called hung.
struct nx::impl::hang_watchdog_config
{
    /// How long one test may run, in seconds; 0 disables it.
    ///
    /// Generous by design.
    /// This is a guard that turns a hang into a message, so a green run never reaches it and its value costs nothing
    /// — while a value tight enough to catch a merely slow test would make the suite flaky on a loaded machine.
    double per_test_secs = 0;

    /// How long the whole run may take, in seconds; 0 disables it.
    /// Catches a run that makes progress forever without any single test overrunning.
    double per_run_secs = 0;

    /// How often the native watchdog looks.
    /// A deadline is therefore accurate to about this, which is far finer than any sensible deadline.
    double poll_interval_secs = 0.25;
};
