#pragma once

#include <clean-core/memory/unique_ptr.hh>

// The run watchdog: a hung run gets a report and a non-zero exit, instead of a silent wait for someone to notice.
//
// One global heartbeat, bumped whenever a test starts, enters another section pass, or finishes.
// A hung test is never done, so once the others drain the heartbeat stops; the delay is only as long as they take.
// Progress rather than a per-test clock is what makes an ASYNC_TEST catchable: one that awaits forever holds no thread,
// so nothing but the missing heartbeat says it is stuck.
//
// **Where it watches from differs by platform.**
// Natively it is a thread outside the run, so it notices a test that spins as readily as one that waits.
// Under wasm the run is stepped from the host's event loop, and the host loop polls instead.
// A body that never returns never gives that loop back, so a spin in straight-line C++ is not caught there, and the
// runner that launched the module is the backstop.

namespace nx::impl
{
/// Records progress: a test started, began another pass, or finished.
/// One relaxed increment, so it may be called from any thread.
void watchdog_heartbeat();

/// The report a run that stopped making progress gets, then the exit.
///
/// What tracked work is outstanding, the running tests, every thread's machine stack and open scopes, and last the
/// recording, written with the consumer paused.
/// A hung test cannot be unwound and no later result would be trustworthy, so this ends the process with code 4.
[[noreturn]] void report_hung_run(double quiet_secs);

/// While alive, watches the heartbeat.
///
/// After `quiet_secs` without one it calls `report`, which defaults to report_hung_run and so does not return.
/// A report that does return is called again after every further `quiet_secs` while nothing moves — how a test
/// observes the watchdog firing.
/// A non-positive `quiet_secs` watches nothing.
struct run_watchdog
{
    explicit run_watchdog(double quiet_secs, void (*report)(double quiet_secs) = nullptr);
    ~run_watchdog();

    run_watchdog(run_watchdog const&) = delete;
    run_watchdog& operator=(run_watchdog const&) = delete;

    /// Checks the heartbeat now, on the calling thread.
    ///
    /// The half for a build with no thread to watch from — the wasm host loop calls it between steps.
    /// Harmless where a watching thread exists as well.
    void poll();

private:
    struct state;
    cc::unique_ptr<state> _state;
};
} // namespace nx::impl
