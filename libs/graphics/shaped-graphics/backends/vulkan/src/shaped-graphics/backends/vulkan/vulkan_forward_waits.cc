#include "vulkan_forward_waits.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/log.hh>
#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/record/domain.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/thread_pump.hh>

using namespace cc::primitive_defines;

namespace
{
struct tracker
{
    int pending_timelines = 0; // timelines whose max_waited is above their signal_submitted
    int barriers = 0;          // device_driver_barriers currently up
};

// How long a barrier waits before it says so.
constexpr double slow_barrier_secs = 5.0;

// Never destroyed: a context torn down during static destruction still reports its last signals here.
struct forward_waits
{
    cc::mutex<tracker> state;
    std::condition_variable changed;
};

forward_waits& g_forward_waits()
{
    static auto* const waits = new forward_waits();
    return *waits;
}

bool is_pending(sg::backend::vulkan::forward_wait_state const& s)
{
    return s.max_waited > s.signal_submitted;
}

void note_wait(tracker& t, sg::backend::vulkan::forward_wait_state& s, u64 value)
{
    auto const was_pending = is_pending(s);
    if (value > s.max_waited)
        s.max_waited = value;
    if (!was_pending && is_pending(s))
        ++t.pending_timelines;
}
} // namespace

void sg::backend::vulkan::before_forward_wait(forward_wait_state& state, u64 value)
{
    auto& waits = g_forward_waits();
#if CC_HAS_THREADS
    // While a barrier is up, hold back until this value's signal is queued: then the wait is never pending at all.
    waits.state.wait(
        waits.changed, [&](tracker const& t) { return t.barriers == 0 || value <= state.signal_submitted; },
        [&](tracker& t) { note_wait(t, state, value); });
#else
    waits.state.lock([&](tracker& t) { note_wait(t, state, value); });
#endif
}

void sg::backend::vulkan::note_signal_submitted(forward_wait_state& state, u64 value)
{
    auto& waits = g_forward_waits();
    waits.state.lock(
        [&](tracker& t)
        {
            auto const was_pending = is_pending(state);
            if (value > state.signal_submitted)
                state.signal_submitted = value;
            if (was_pending && !is_pending(state))
                --t.pending_timelines;
        });
    waits.changed.notify_all();
}

sg::backend::vulkan::device_driver_barrier::device_driver_barrier()
{
    auto& waits = g_forward_waits();
    waits.state.lock([](tracker& t) { ++t.barriers; });
#if CC_HAS_THREADS
    auto const nothing_pending = [](tracker const& t) { return t.pending_timelines == 0; };
    if (!waits.state.wait_for(waits.changed, slow_barrier_secs, nothing_pending))
    {
        CC_LOG_WARNING("creating or destroying a Vulkan device has waited {:g} s for pending wait-before-signal to be "
                       "matched by their signals — usually a stream whose source has not delivered its last bytes, "
                       "which a submitted command list waits on",
                       slow_barrier_secs);
        waits.state.wait(waits.changed, nothing_pending, [](tracker&) {});
    }
#else
    while (waits.state.lock([](tracker const& t) { return t.pending_timelines > 0; }))
    {
        auto const progressed = cc::thread_pump_all();
        CC_ASSERT(progressed, "a wait-before-signal is pending and nothing is left to signal it");
        if (!progressed)
            break;
    }
#endif
}

sg::backend::vulkan::device_driver_barrier::~device_driver_barrier()
{
    auto& waits = g_forward_waits();
    waits.state.lock([](tracker& t) { --t.barriers; });
    waits.changed.notify_all();
}

int sg::backend::vulkan::device_driver_barriers_up()
{
    return g_forward_waits().state.lock([](tracker const& t) { return t.barriers; });
}

int sg::backend::vulkan::pending_forward_wait_timelines()
{
    return g_forward_waits().state.lock([](tracker const& t) { return t.pending_timelines; });
}
