#pragma once

#include <clean-core/memory/unique_ptr.hh>

// The run watchdog: a hung test gets every thread's stack on stderr, instead of a silent wait for someone to notice.
//
// One global heartbeat, bumped whenever a test starts, enters another section pass, or finishes.
// A hung test is never done, so once the others drain the heartbeat stops; the delay is only as long as they take.

namespace nx::impl
{
/// Records progress: a test started, began another pass, or finished.
/// One relaxed increment, so it may be called from any thread.
void watchdog_heartbeat();

/// While alive, watches the heartbeat from a thread of its own.
/// After `quiet_secs` without one it prints a `[nexus watchdog]` marker and every thread's stack to stderr, and again after every further `quiet_secs` while nothing moves.
/// It only reports, and never ends the run.
/// A non-positive `quiet_secs` watches nothing, and neither does a build without threads.
struct run_watchdog
{
    /// `report`, when given, replaces the stderr report — how a test observes the watchdog firing.
    explicit run_watchdog(double quiet_secs, void (*report)(double quiet_secs) = nullptr);
    ~run_watchdog();

    run_watchdog(run_watchdog const&) = delete;
    run_watchdog& operator=(run_watchdog const&) = delete;

private:
    struct state;
    cc::unique_ptr<state> _state;
};
} // namespace nx::impl
