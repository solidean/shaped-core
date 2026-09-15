#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/thread/thread_pump.hh>

// cc::impl::async_parker — where a blocking drive waits once it has nothing left to run.

namespace cc::impl
{
struct async_parker_state;

/// Parks a thread that drives `root` and found nothing to run, until something that could unblock it happens.
///
/// That is one of three things, and never a timeout:
///   - `root` resolved,
///   - a registered pump signalled work (cc::thread_pump_notify), when this park drives pumps,
///   - `home` received work, when the thread has a home it may run.
///
/// Only a loop that owns its thread drives pumps — a blocking drive on a scheduler without threads, nexus's run loop.
/// A thread parked in a pool passes `drives_pumps = false`, since an unrelated component's handlers must not land on it.
///
/// Construct it once per drive and call park() in the drive's loop.
/// The completion latch it installs on `root` lives as long as the root does, so the state it points at is shared with
/// the latch rather than owned by this frame.
/// Without threads nothing parks: park() sweeps the registry once and returns.
class async_parker
{
public:
    /// `home` is the thread home to wait on as well, or null: pass null from inside one of that home's own bodies, whose
    /// queue is not this wait's to run.
    async_parker(async_node_base& root, thread_bound_scheduler* home, bool drives_pumps);
    ~async_parker();

    async_parker(async_parker const&) = delete;
    async_parker& operator=(async_parker const&) = delete;

    /// Sweeps the pump registry when this park drives pumps, and parks only when that sweep found nothing.
    /// Returns once the caller should look again; a spurious return is harmless.
    void park();

    /// park(), giving up after `max_secs` — for a drive whose caller asked for a deadline.
    void park_for(double max_secs);

    /// Whether the root's completion latch has fired.
    [[nodiscard]] bool is_root_done() const;

private:
    async_parker_state* _state = nullptr;
};
} // namespace cc::impl
