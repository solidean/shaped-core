#include <clean-core/common/assert.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/async_node.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/impl/async_parker.hh>
#include <clean-core/thread/thread_bound_scheduler.hh>

#if CC_HAS_THREADS
#include <condition_variable>
#include <mutex>
#endif

namespace cc::impl
{
struct async_parker_state
{
#if CC_HAS_THREADS
    std::mutex m;
    std::condition_variable cv;
    bool root_done = false;                 // under m
    bool woken = false;                     // under m: a pump signalled since the last park began
    thread_bound_scheduler* home = nullptr; // under m; cleared when the parker goes, so a late latch wakes nobody
    bool drives_pumps = false;

    // The parker holds one, the root's latch one more; whoever drops the last frees the state.
    cc::atomic<int> refs = 1;

    static void release(async_parker_state* s)
    {
        if (s->refs.fetch_sub(1) == 1)
            delete s;
    }

    static void on_root_done(void* p)
    {
        auto* const s = static_cast<async_parker_state*>(p);
        {
            std::lock_guard const lock(s->m);
            s->root_done = true;
            if (s->home != nullptr)
                s->home->wake();
            s->cv.notify_all();
        }
        release(s);
    }

    static void on_pump_notify(void* p)
    {
        auto* const s = static_cast<async_parker_state*>(p);
        std::lock_guard const lock(s->m);
        s->woken = true;
        if (s->home != nullptr)
            s->home->wake();
        s->cv.notify_all();
    }

    // Declared last, so it is removed from the listener list before anything it reaches is torn down.
    cc::unique_ptr<thread_pump_listener> listener;
#else
    bool root_done = false;
    bool drives_pumps = false;
#endif
};
} // namespace cc::impl

cc::impl::async_parker::async_parker(async_node_base& root, thread_bound_scheduler* home, bool drives_pumps)
  : _state(new async_parker_state())
{
    CC_ASSERT(home == nullptr || home->is_owner_thread(), "a drive can wait on only the calling thread's own home");
#if CC_HAS_THREADS
    _state->home = home;
    _state->drives_pumps = drives_pumps;
    if (drives_pumps)
        _state->listener = cc::make_unique<thread_pump_listener>(&async_parker_state::on_pump_notify, _state);

    _state->refs.fetch_add(1); // the latch's, handed back below if the root was already done
    if (root.install_completion_hook_or_ready(&async_parker_state::on_root_done, _state))
    {
        _state->root_done = true;
        _state->refs.fetch_sub(1);
    }
#else
    CC_UNUSED(home);
    _state->drives_pumps = drives_pumps;
    _state->root_done = root.is_ready();
#endif
}

cc::impl::async_parker::~async_parker()
{
#if CC_HAS_THREADS
    _state->listener = nullptr;
    {
        std::lock_guard const lock(_state->m);
        _state->home = nullptr;
    }
    async_parker_state::release(_state);
#else
    delete _state;
#endif
}

bool cc::impl::async_parker::is_root_done() const
{
#if CC_HAS_THREADS
    std::lock_guard const lock(_state->m);
#endif
    return _state->root_done;
}

void cc::impl::async_parker::park()
{
    park_for(-1);
}

void cc::impl::async_parker::park_for(double max_secs)
{
#if CC_HAS_THREADS
    auto* home = static_cast<thread_bound_scheduler*>(nullptr);
    {
        std::lock_guard const lock(_state->m);
        if (_state->root_done)
            return;
        _state->woken = false;
        home = _state->home;
    }

    // The sweep comes after `woken` was cleared, which is what makes a notify racing this park impossible to lose:
    // work posted before the clear is visible to the sweep, and a notify after it sets the flag the wait below reads.
    if (_state->drives_pumps && cc::impl::thread_pump_registry())
        return;

    if (home != nullptr)
    {
        // The home's own wait, since its work arrives there; the latch and the listener wake it too.
        home->wait_for_work_or_wake(max_secs);
        return;
    }

    std::unique_lock lock(_state->m);
    auto const released = [&] { return _state->root_done || _state->woken; };
    if (max_secs < 0)
        _state->cv.wait(lock, released);
    else if (!released())
        cc::impl::condition_wait_secs(_state->cv, lock, max_secs);
#else
    CC_UNUSED(max_secs);
    if (_state->drives_pumps)
        (void)cc::impl::thread_pump_registry();
#endif
}
