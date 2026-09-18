#include "completion_waiter.hh"

#include <clean-core/common/utility.hh>

#include <thread>

namespace sg::impl
{
struct completion_waiter::state
{
    hooks h;

    struct armed_targets
    {
        u64 submission = 0;
        u64 epoch = 0;
        // Strictly increasing, because a vulkan timeline rejects a host signal that does not raise it.
        u64 generation = 0;
        bool is_started = false;
        bool is_stopped = false;
    };
    cc::mutex<armed_targets> armed;

#if CC_HAS_THREADS
    std::thread waiter;
#else
    cc::thread_pump_registration pump;
#endif

    void run();
    bool pump_once();
};

completion_waiter::completion_waiter(hooks h) : _state(std::make_unique<state>())
{
    _state->h = cc::move(h);
}

completion_waiter::~completion_waiter()
{
    stop();
}

void completion_waiter::arm(u64 submission, u64 epoch)
{
    auto& s = *_state;
    auto const start = s.armed.lock(
        [&](state::armed_targets& a)
        {
            if (a.is_stopped)
                return false;

            auto const lowers = [](u64 fresh, u64 old) { return fresh != 0 && (old == 0 || fresh < old); };
            auto const needs_wake = lowers(submission, a.submission) || lowers(epoch, a.epoch);
            a.submission = submission;
            a.epoch = epoch;

            if (!a.is_started)
            {
                if (submission == 0 && epoch == 0)
                    return false;
                a.is_started = true;
                return true;
            }
#if CC_HAS_THREADS
            // Under the lock, so a generation handed to the backend is never older than one it already saw.
            if (needs_wake)
                s.h.wake(++a.generation);
#else
            // Nothing parks on a wake without threads: the pump's GPU wait is the only one.
            CC_UNUSED(needs_wake);
#endif
            return false;
        });

    if (!start)
        return;
#if CC_HAS_THREADS
    s.waiter = std::thread([&s] { s.run(); });
#else
    s.pump = cc::register_thread_pump([&s] { return s.pump_once(); });
#endif
}

void completion_waiter::stop()
{
    auto& s = *_state;
    // Started is what decides the join, not who stopped it: a waiter that saw its device lost stops itself and still has to be joined.
    auto const was_running = s.armed.lock(
        [&](state::armed_targets& a)
        {
            if (!a.is_stopped)
            {
                a.is_stopped = true;
#if CC_HAS_THREADS
                if (a.is_started)
                    s.h.wake(++a.generation);
#endif
            }
            return a.is_started;
        });
    if (!was_running)
        return;
#if CC_HAS_THREADS
    if (s.waiter.joinable())
        s.waiter.join();
#else
    s.pump = cc::thread_pump_registration();
#endif
}

void completion_waiter::state::run()
{
    while (true)
    {
        // The generation is read with the targets, so a wake that lowers a target after this line is never missed.
        auto const a = armed.lock([](armed_targets& t) { return t; });
        if (a.is_stopped)
            return;

        h.park(a.submission, a.epoch, a.generation);
        h.settle();

        // A lost device settles everything as an error above, and its signals may never fire again.
        if (h.is_device_lost())
        {
            armed.lock([](armed_targets& t) { t.is_stopped = true; });
            h.settle();
            return;
        }
    }
}

bool completion_waiter::state::pump_once()
{
    auto const has_targets = armed.lock([](armed_targets& t) { return t.submission != 0 || t.epoch != 0; });
    if (!has_targets)
        return false;

    h.settle();

    // Only what the GPU has been handed can signal: parking on the open epoch would never return.
    auto const open_epoch = h.open_epoch();
    auto a = armed.lock([](armed_targets& t) { return t; });
    if (a.epoch >= open_epoch)
        a.epoch = 0;
    if (a.submission == 0 && a.epoch == 0)
        return false; // nothing the GPU could signal: drains settle from their actors, open epochs after an advance

    // A GPU target may itself wait on a copy only a sibling actor signals, so every sibling runs before this parks.
    if (cc::thread_pump_all())
        return true;

    h.park(a.submission, a.epoch, 0);
    h.settle();
    return true;
}
} // namespace sg::impl
