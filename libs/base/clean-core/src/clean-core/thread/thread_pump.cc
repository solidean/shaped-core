#include <clean-core/common/assert.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <clean-core/thread/spin.hh>
#include <clean-core/thread/thread_pump.hh>

#include <chrono>

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

    return thread_pump_registration(entry);
}

bool cc::thread_pump_all()
{
    if (g_registration_count.load() == 0)
        return false;

    // Snapshot under the lock, call outside it: a pump is free to register or deregister — an actor handler creating
    // another actor does exactly that — and holding the lock across the call would deadlock on it.
    auto snapshot = cc::vector<impl::thread_pump_entry*>();
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

    auto more = false;
    for (auto* const entry : snapshot)
    {
        if (entry->running.exchange(true))
            continue; // busy: either this same thread further up the stack, or another one

        if (entry->alive.load())
            more |= entry->pump();

        entry->running.store(false);
    }

    for (auto* const entry : snapshot)
        release_entry(entry);

    return more;
}

bool cc::thread_pump_all_for(double max_ms)
{
    if (max_ms <= 0)
        return thread_pump_all();

    auto const deadline = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(max_ms);
    while (true)
    {
        if (!thread_pump_all())
            return false; // idle: nothing left to do
        if (std::chrono::steady_clock::now() >= deadline)
            return true; // stopped on the budget with work still pending
    }
}

isize cc::registered_thread_pump_count()
{
    return isize(g_registration_count.load());
}
