#include <clean-core/common/assert.hh>
#include <clean-core/common/time.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/impl/async_tls.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/spin.hh>
#include <clean-core/thread/thread_bound_scheduler.hh>
#include <clean-core/thread/thread_pump.hh>

using namespace cc::primitive_defines;

namespace cc::impl
{
/// One registered pump, kept at a stable address so a sweep can hold it across the unlocked call.
struct thread_pump_entry
{
    cc::unique_function<bool()> pump;

    /// Claimed for the duration of the call.
    /// Reentrancy guard and cross-thread guard at once: a semantic thread that is busy takes no new work either way.
    cc::atomic<bool> running = false;

    /// Cleared by deregistration, and read only after `running` is claimed.
    /// The two together are what make a deregistration safe against a sweep that is already mid-snapshot — see the
    /// ordering argument in reset().
    cc::atomic<bool> alive = true;

    /// Set by a sweep that found the pump busy.
    /// The thread inside the pump runs it again before leaving, since the work that sweep came for may have landed after
    /// its own last look — and the sweeper, finding it busy, may be about to park.
    cc::atomic<bool> rerun = false;

    /// The registration holds one; each in-flight sweep holds one more.
    /// Whoever drops the last frees the entry.
    cc::atomic<int> refs = 1;
};
} // namespace cc::impl

namespace
{
struct registry_state
{
    cc::vector<cc::impl::thread_pump_entry*> entries;
};

cc::mutex<registry_state>& pump_registry()
{
    // Deliberately never destroyed: an actor may deregister during static destruction, and a destroyed registry is a
    // worse outcome than a leaked one.
    static auto* const registry = new cc::mutex<registry_state>();
    return *registry;
}

/// Live registrations, readable without the lock.
/// This is the fast path in full: a threaded build registers nothing, so a sweep is this load and nothing else.
cc::atomic<int> g_registration_count = 0;

/// The threads that may be parked waiting for a pump to have work.
/// Immortal for the registry's reason.
cc::mutex<cc::vector<cc::impl::thread_pump_listener*>>& pump_listeners()
{
    static auto* const listeners = new cc::mutex<cc::vector<cc::impl::thread_pump_listener*>>();
    return *listeners;
}

/// The pumps this thread is inside, innermost last.
/// A sweep from inside a pump finds that pump busy on its own stack, which is no reason to run it again.
thread_local cc::vector<cc::impl::thread_pump_entry*> t_running_here;

/// Runs the pump, again for as long as a concurrent sweep asked for a rerun while it ran.
/// The caller has claimed `running`; this releases it.
bool run_claimed(cc::impl::thread_pump_entry& entry)
{
    auto more = false;
    while (true)
    {
        entry.rerun.store(false);
        if (entry.alive.load())
        {
            t_running_here.push_back(&entry);
            CC_DEFER
            {
                t_running_here.remove_back();
            };
            more |= entry.pump();
        }

        entry.running.store(false);

        // A sweep that found us busy set the flag before trying to claim, so either it claims now or we see the flag.
        if (!entry.rerun.load() || entry.running.exchange(true))
            return more;
    }
}

void release_entry(cc::impl::thread_pump_entry* entry)
{
    if (entry->refs.fetch_sub(1) == 1)
        delete entry;
}
} // namespace

cc::thread_pump_registration::thread_pump_registration(thread_pump_registration&& rhs) noexcept : _entry(rhs._entry)
{
    rhs._entry = nullptr;
}

cc::thread_pump_registration& cc::thread_pump_registration::operator=(thread_pump_registration&& rhs) noexcept
{
    if (this != &rhs)
    {
        reset();
        _entry = rhs._entry;
        rhs._entry = nullptr;
    }
    return *this;
}

cc::thread_pump_registration::~thread_pump_registration()
{
    reset();
}

void cc::thread_pump_registration::reset()
{
    if (_entry == nullptr)
        return;

    auto* const entry = _entry;
    _entry = nullptr;

    // Ordered before the spin below, and paired with the sweep claiming `running` before it reads `alive`.
    // Under that pairing one of the two always sees the other: either the sweep reads `alive` as false and never calls,
    // or it claimed `running` first and the spin waits the call out.
    entry->alive.store(false);

    pump_registry().lock([&](registry_state& state) { (void)state.entries.remove_first_value(entry); });
    g_registration_count.fetch_sub(1);

    // The semantic thread this belongs to is being torn down, so returning while a sweep is still inside its pump would
    // hand that sweep a destroyed actor.
    while (entry->running.load())
        cc::spin_pause();

    release_entry(entry);
}

cc::thread_pump_registration cc::register_thread_pump(cc::unique_function<bool()> pump)
{
    CC_ASSERT(pump.is_valid(), "a registered pump must be callable");

    auto* const entry = new impl::thread_pump_entry{.pump = cc::move(pump)};
    pump_registry().lock([&](registry_state& state) { state.entries.push_back(entry); });
    g_registration_count.fetch_add(1);

    // The new pump may hold work already, and a thread parked before it existed would never sweep it.
    thread_pump_notify();

    return thread_pump_registration(entry);
}

void cc::thread_pump_notify()
{
    // Every listener, under the list's lock: a listener's destructor takes the same lock, so none is called after it died.
    pump_listeners().lock(
        [](cc::vector<impl::thread_pump_listener*>& listeners)
        {
            for (auto* const l : listeners)
                l->wake(l->ctx);
        });
}

cc::impl::thread_pump_listener::thread_pump_listener(void (*wake_fn)(void*), void* wake_ctx)
  : wake(wake_fn), ctx(wake_ctx)
{
    CC_ASSERT(wake != nullptr, "a pump listener needs a wake function");
    pump_listeners().lock([&](cc::vector<thread_pump_listener*>& listeners) { listeners.push_back(this); });
}

cc::impl::thread_pump_listener::~thread_pump_listener()
{
    pump_listeners().lock([&](cc::vector<thread_pump_listener*>& listeners)
                          { (void)listeners.remove_first_value(this); });
}

bool cc::impl::thread_pump_registry()
{
    if (g_registration_count.load() == 0)
        return false;

    auto more = false;

    // Snapshot under the lock, call outside it: a pump is free to register or deregister — an actor handler creating
    // another actor does exactly that — and holding the lock across the call would deadlock on it.
    auto snapshot = cc::vector<cc::impl::thread_pump_entry*>();
    pump_registry().lock(
        [&](registry_state& state)
        {
            snapshot.reserve(state.entries.size());
            for (auto* const entry : state.entries)
            {
                entry->refs.fetch_add(1);
                snapshot.push_back(entry);
            }
        });

    for (auto* const entry : snapshot)
    {
        // Busy: this same thread further up the stack, or another one.
        // Asked to run again rather than skipped, because whatever this sweep came for may have landed after the
        // running thread's last look — and a sweeper that finds nothing parks.
        auto running_here = false;
        for (auto const* const here : t_running_here)
            running_here |= here == entry;
        if (running_here)
            continue;
        entry->rerun.store(true);
        if (entry->running.exchange(true))
            continue;
        more |= run_claimed(*entry);
    }

    for (auto* const entry : snapshot)
        release_entry(entry);

    return more;
}

bool cc::thread_pump_all()
{
    // The calling thread's own home first: every blocking wait that sweeps here then also runs the homed steps only this
    // thread may run, without the home ever sitting in the registry where every other thread's sweep would pay for it.
    auto more = false;
    if (auto* const home = cc::impl::async_tls().home)
        more = home->pump_cycle();
    more |= cc::impl::thread_pump_registry();
    return more;
}

bool cc::thread_pump_all_for(double max_ms)
{
    if (max_ms <= 0)
        return thread_pump_all();

    auto const deadline = cc::current_time_steady_secs() + max_ms / 1000.0;
    while (true)
    {
        if (!thread_pump_all())
            return false; // idle: nothing left to do
        if (cc::current_time_steady_secs() >= deadline)
            return true; // stopped on the budget with work still pending
    }
}

isize cc::registered_thread_pump_count()
{
    return isize(g_registration_count.load());
}
