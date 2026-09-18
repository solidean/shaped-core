#pragma once

#include <clean-core/fwd.hh>
#include <shaped-graphics/fwd.hh>

// Keeping device creation and teardown clear of pending wait-before-signal, across every backend.
//
// This exists for a driver bug, not for anything sg's own design requires.
// NVIDIA's vkCreateDevice waits for the GPU to go idle while holding a lock that every way of signalling needs.
// A queue already waiting on a value nobody has submitted a signal for can then never go idle, and the process deadlocks
// in the kernel: docs/bugs-external/nvidia-device-create-vs-pending-wait-deadlock reproduces it with nothing of ours.
//
// sg makes such waits routinely.
// An async transfer reserves its completion value at enqueue, and a later submission — a command list, or the other
// transfer actor — waits on it before the actor that owns it has submitted the copy that signals it.
// So each completion timeline carries a forward_wait_state, every submission that waits on one says so first, every
// signal is reported once queued, and the driver's device create and destroy calls run inside a device_driver_barrier.
//
// The barrier never makes a signaller wait.
// While one is up, a submission that would wait on an unsignalled value holds itself back until that value's signal
// has been submitted; the barrier waits for the waits already queued to be matched by their signals.
// Signals come from the transfer actors, whose own waits only ever name each other's signals and never form a cycle,
// so both sides always finish.

namespace sg::impl
{
struct forward_wait_state;
class device_driver_barrier;

/// Before queueing a submission that waits on `value` of the timeline `state` belongs to.
/// Returns once the wait is safe to queue: counted as a pending wait-before-signal, or — while a device_driver_barrier
/// is up — once the signal of `value` has itself been submitted, so no new one appears across the driver call.
/// Without threads it never blocks, since the barrier's own thread is the one that must submit the signal.
void before_forward_wait(forward_wait_state& state, u64 value);

/// After a submission that signals `value` on the timeline `state` belongs to has been queued.
void note_signal_submitted(forward_wait_state& state, u64 value);
} // namespace sg::impl

/// One completion timeline's wait-before-signal bookkeeping, kept beside the timeline.
/// Both fields only ever rise, and only under the process-wide lock behind the functions above.
struct sg::impl::forward_wait_state
{
    u64 max_waited = 0;       // the highest value any queued submission waits on
    u64 signal_submitted = 0; // the highest value whose signal has been queued
};

/// Around the driver's own device creation or destruction call, under a device_lifecycle_hold.
/// Construction holds new wait-before-signal back and waits until every one already queued is matched by its signal.
/// Without threads nothing else can submit those signals, so it drives the transfer actors itself through
/// cc::thread_pump_all until they have.
class sg::impl::device_driver_barrier
{
public:
    device_driver_barrier();
    ~device_driver_barrier();

    device_driver_barrier(device_driver_barrier const&) = delete;
    device_driver_barrier& operator=(device_driver_barrier const&) = delete;
};
