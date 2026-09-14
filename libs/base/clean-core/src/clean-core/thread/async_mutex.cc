#include <clean-core/thread/async_mutex.hh>
#include <clean-core/thread/spin.hh>

using namespace cc::primitive_defines;

void cc::impl::async_permit_hold::release()
{
    if (_core == nullptr)
        return;
    auto* const core = _core;
    _core = nullptr;
    core->release(_permits);
}

cc::impl::async_permit_core::async_permit_core(isize capacity) : _capacity(capacity), _state(u64(capacity))
{
    CC_ASSERT(capacity > 0 && (u64(capacity) & waiters_bit) == 0, "an async lock needs a positive permit count");
    _spin.clear();
}

cc::impl::async_permit_core::~async_permit_core()
{
    CC_ASSERT((_state.load(cc::memory_order_acquire) & available_mask) == u64(_capacity),
              "an async mutex or semaphore was destroyed while still held");
}

void cc::impl::async_permit_core::lock_spin()
{
    while (_spin.test_and_set(cc::memory_order_acquire))
        cc::spin_pause();
}

void cc::impl::async_permit_core::unlock_spin()
{
    _spin.clear(cc::memory_order_release);
}

bool cc::impl::async_permit_core::try_acquire(isize permits)
{
    auto s = _state.load(cc::memory_order_acquire);
    for (;;)
    {
        if ((s & available_mask) < u64(permits))
            return false;
        if ((s & waiters_bit) != 0)
            break;
        if (_state.compare_exchange_weak(s, s - u64(permits), cc::memory_order_acq_rel, cc::memory_order_acquire))
            return true;
    }

    // The permits are free and somebody is queued, which only a dropped waiter at the head can explain for long.
    // Settle the queue, then look again: a live waiter still takes precedence, so nobody barges past the queue.
    cc::vector<granted> to_push;
    auto taken = false;
    lock_spin();
    dispatch_locked(to_push);
    s = _state.load(cc::memory_order_acquire);
    while ((s & waiters_bit) == 0 && (s & available_mask) >= u64(permits))
        if (_state.compare_exchange_weak(s, s - u64(permits), cc::memory_order_acq_rel, cc::memory_order_acquire))
        {
            taken = true;
            break;
        }
    unlock_spin();
    push_granted(to_push);
    return taken;
}

bool cc::impl::async_permit_core::acquire_or_enqueue(isize permits, async_node_base* grant, push_fn push, void* context)
{
    cc::vector<granted> to_push;
    lock_spin();

    // A dropped waiter at the head would otherwise hold this arrival back until the next release, which for a reader
    // behind a dead writer may never come.
    if ((_state.load(cc::memory_order_acquire) & waiters_bit) != 0)
        dispatch_locked(to_push);

    auto s = _state.load(cc::memory_order_acquire);
    for (;;)
    {
        if ((s & waiters_bit) == 0 && (s & available_mask) >= u64(permits))
        {
            // Released between the caller's fast try and this lock, or freed by the dispatch above: take them after all.
            if (_state.compare_exchange_weak(s, s - u64(permits), cc::memory_order_acq_rel, cc::memory_order_acquire))
            {
                unlock_spin();
                push_granted(to_push);
                return true;
            }
            continue;
        }

        // Announce the waiter in the same CAS discipline every release uses, so a release racing this either lands
        // before it (and the loop sees the permits) or sees the bit and takes the queue path under the spinlock.
        if (_state.compare_exchange_weak(s, s | waiters_bit, cc::memory_order_acq_rel, cc::memory_order_acquire))
            break;
    }

    _queue.push_back(
        waiter{.grant = async_node_weak::from_alive(grant), .permits = permits, .push = push, .context = context});
    unlock_spin();
    push_granted(to_push);
    return false;
}

void cc::impl::async_permit_core::release(isize permits)
{
    auto s = _state.load(cc::memory_order_acquire);
    while ((s & waiters_bit) == 0)
        if (_state.compare_exchange_weak(s, s + u64(permits), cc::memory_order_acq_rel, cc::memory_order_acquire))
            return;

    cc::vector<granted> to_push;

    lock_spin();
    s = _state.load(cc::memory_order_acquire);
    while (!_state.compare_exchange_weak(s, s + u64(permits), cc::memory_order_acq_rel, cc::memory_order_acquire))
    {
    }
    dispatch_locked(to_push);
    unlock_spin();

    push_granted(to_push);
}

void cc::impl::async_permit_core::dispatch_locked(cc::vector<granted>& to_push)
{
    // Hand the free permits to the oldest live waiters, head of line first.
    // The strong handle taken here is what makes the push safe: a waiter dropped after this still has its grant alive,
    // and dropping that grant later destroys the guard, which releases the permits again.
    while (_head < _queue.size())
    {
        auto& w = _queue[_head];
        auto strong = w.grant.lock();
        if (strong == nullptr)
        {
            w.grant = {}; // the waiter was dropped before its turn: nothing to hand over
            ++_head;
            continue;
        }

        auto s = _state.load(cc::memory_order_acquire);
        if ((s & available_mask) < u64(w.permits))
            break; // the head does not fit yet; nobody behind it may jump the queue

        while (!_state.compare_exchange_weak(s, s - u64(w.permits), cc::memory_order_acq_rel, cc::memory_order_acquire))
        {
        }
        to_push.push_back(granted{.grant = cc::move(strong), .permits = w.permits, .push = w.push, .context = w.context});
        w.grant = {}; // a consumed slot's weak count would otherwise pin the grant node's storage until the queue empties
        ++_head;
    }

    if (_head == _queue.size())
    {
        _queue.clear();
        _head = 0;
        auto s = _state.load(cc::memory_order_acquire);
        while (!_state.compare_exchange_weak(s, s & ~waiters_bit, cc::memory_order_acq_rel, cc::memory_order_acquire))
        {
        }
    }
    else if (_head * 2 >= _queue.size())
    {
        // Under steady contention the queue never empties — every release re-queues its releaser — so the consumed
        // prefix is dropped once it is half the queue, which keeps it within twice the entries still waiting.
        _queue.remove_at_range({.offset = 0, .size = _head});
        _head = 0;
    }
}

void cc::impl::async_permit_core::push_granted(cc::vector<granted>& to_push)
{
    // Outside the spinlock: a push wakes the waiter, and dropping a grant whose waiter has gone releases permits again.
    for (auto& g : to_push)
        g.push(g.grant.get(), async_permit_hold(this, g.permits), g.context);
}

isize cc::impl::async_permit_core::queued_slots()
{
    lock_spin();
    auto const n = _queue.size();
    unlock_spin();
    return n;
}
