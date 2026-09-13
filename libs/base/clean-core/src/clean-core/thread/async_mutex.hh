#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>
#include <clean-core/thread/async.hh>
#include <clean-core/thread/async_coroutine.hh>
#include <clean-core/thread/atomic.hh>

// cc::async_mutex<T>, cc::async_shared_mutex<T> and cc::async_semaphore — exclusion that parks a waiting async instead of blocking its thread.
//
// A contended lock suspends the waiting node, and the thread goes on running other work; the unlock hands the lock to the oldest waiter and schedules it.
// A guard may be held across a co_await and released on whichever thread the node resumes on.
// With threads off this is still real exclusion: a holder suspended across an await contends with every other node that wants the lock.
// The model, the policy and the deadlock rules are "Exclusion" in libs/base/clean-core/docs/systems/async.md.

namespace cc::impl
{
struct async_permit_core;

/// A hold on `permits` of a core, released on destruction on whatever thread that is.
/// Move-only; a moved-from hold releases nothing.
struct async_permit_hold
{
    async_permit_hold() = default;
    async_permit_hold(async_permit_core* core, isize permits) : _core(core), _permits(permits) {}
    async_permit_hold(async_permit_hold&& rhs) noexcept : _core(rhs._core), _permits(rhs._permits)
    {
        rhs._core = nullptr;
    }
    async_permit_hold& operator=(async_permit_hold&& rhs) noexcept
    {
        if (this != &rhs)
        {
            release();
            _core = rhs._core;
            _permits = rhs._permits;
            rhs._core = nullptr;
        }
        return *this;
    }
    async_permit_hold(async_permit_hold const&) = delete;
    async_permit_hold& operator=(async_permit_hold const&) = delete;
    ~async_permit_hold() { release(); }

    [[nodiscard]] bool is_held() const { return _core != nullptr; }

    void release();

private:
    async_permit_core* _core = nullptr;
    isize _permits = 0;
};

/// The counted, FIFO, head-of-line permit queue all three primitives are.
///
/// A mutex is one permit; a shared mutex gives a reader one permit and a writer all of them, which is what makes a
/// queued writer block the readers behind it; a semaphore is its count.
/// The uncontended acquire and release are one CAS each; everything else happens under a spinlock.
///
/// A waiter is a grant node the queue holds WEAKLY: unlock pushes a live hold into the oldest one still alive.
/// A waiter that was dropped simply fails to lock and is skipped, and one dropped after its grant releases the hold
/// through the grant's value teardown — so cancelling a wait needs no unlinking at all.
struct async_permit_core
{
    explicit async_permit_core(isize capacity);
    ~async_permit_core();

    async_permit_core(async_permit_core const&) = delete;
    async_permit_core& operator=(async_permit_core const&) = delete;

    /// One CAS; false when the permits are not free, or anyone is already waiting.
    [[nodiscard]] bool try_acquire(isize permits);

    using push_fn = void (*)(async_node_base* grant, async_permit_hold hold, void* context);

    /// Queue a waiter for `permits`; `grant` is resolved with the hold once it is this waiter's turn.
    /// `push` builds the typed guard into the grant node from the hold and `context`, which is how the core stays untyped.
    /// Returns true when the permits were free after all, in which case nothing was queued and the caller owns them.
    [[nodiscard]] bool acquire_or_enqueue(isize permits, async_node_base* grant, push_fn push, void* context);

    void release(isize permits);

    [[nodiscard]] isize capacity() const { return _capacity; }

private:
    struct waiter
    {
        async_node_weak grant;
        isize permits;
        push_fn push;
        void* context;
    };

    void lock_spin();
    void unlock_spin();

    static constexpr u64 waiters_bit = u64(1) << 63;
    static constexpr u64 available_mask = ~waiters_bit;

    isize const _capacity;

    /// Free permits in the low bits, and whether anyone waits in the top bit; every write is a CAS.
    cc::atomic<u64> _state;
    cc::atomic_flag _spin;

    /// FIFO consumed from _head; guarded by _spin.
    cc::vector<waiter> _queue;
    isize _head = 0;
};

/// What an awaited acquire hands back: the typed guard, from the fast path or from the grant.
template <class Guard>
struct async_permit_awaiter
{
    cc::optional<Guard> ready;
    cc::shared_async<Guard> grant;

    [[nodiscard]] bool await_ready() const { return ready.has_value(); }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        auto& p = h.promise();
        CC_ASSERT(p.ctx != nullptr, "co_await outside of a compute step");
        if (p.ctx->require(grant))
            return false;
        p.suspend_on(nullptr, nullptr); // a grant never fails, so there is nothing to check before resuming
        return true;
    }

    [[nodiscard]] Guard await_resume()
    {
        if (ready.has_value())
            return cc::move(ready.value());
        return grant->take_value();
    }
};

/// Builds a Guard from a hold and a context pointer: Guard(hold, V*) for a guard that reaches a value, Guard(hold) otherwise.
template <class Guard, class V>
[[nodiscard]] Guard async_make_permit_guard(async_permit_hold hold, void* context)
{
    if constexpr (std::is_void_v<V>)
        return Guard(cc::move(hold));
    else
        return Guard(cc::move(hold), static_cast<V*>(context));
}

template <class Guard, class V>
void async_permit_push(async_node_base* grant, async_permit_hold hold, void* context)
{
    static_cast<cc::async<Guard>*>(grant)->push_value(async_make_permit_guard<Guard, V>(cc::move(hold), context));
}

/// Take `permits` now, if they are free and nobody waits.
template <class Guard, class V>
[[nodiscard]] cc::optional<Guard> async_permit_try(async_permit_core& core, isize permits, void* context)
{
    if (!core.try_acquire(permits))
        return {};
    return async_make_permit_guard<Guard, V>(async_permit_hold(&core, permits), context);
}

/// The acquire as an async: a born-ready grant when the permits are free, else a queued one.
template <class Guard, class V>
[[nodiscard]] cc::shared_async<Guard> async_permit_acquire_async(async_permit_core& core, isize permits, void* context)
{
    if (core.try_acquire(permits))
        return cc::make_async_from_value(async_make_permit_guard<Guard, V>(async_permit_hold(&core, permits), context));

    auto grant = cc::make_async_manual<Guard>();
    if (core.acquire_or_enqueue(permits, grant.get(), &async_permit_push<Guard, V>, context))
        grant->push_value(async_make_permit_guard<Guard, V>(async_permit_hold(&core, permits), context));
    return grant;
}

/// The awaited acquire: the guard directly when the permits are free, else a queued grant.
template <class Guard, class V>
[[nodiscard]] async_permit_awaiter<Guard> async_permit_acquire(async_permit_core& core, isize permits, void* context)
{
    if (core.try_acquire(permits))
        return {.ready = async_make_permit_guard<Guard, V>(async_permit_hold(&core, permits), context)};
    return {.grant = async_permit_acquire_async<Guard, V>(core, permits, context)};
}
} // namespace cc::impl

/// A hold on an async_mutex, reaching the protected value; releasing it hands the lock to the oldest waiter.
/// Move-only, and may be destroyed on any thread.
template <class T>
struct cc::async_mutex_guard
{
    [[nodiscard]] T& operator*() const { return *_value; }
    [[nodiscard]] T* operator->() const { return _value; }

    async_mutex_guard(async_mutex_guard&&) noexcept = default;
    async_mutex_guard& operator=(async_mutex_guard&&) noexcept = default;

    /// Releases early; the guard no longer reaches the value.
    void unlock()
    {
        _hold.release();
        _value = nullptr;
    }

    // not for callers: built by the mutex
    async_mutex_guard(impl::async_permit_hold hold, T* value) : _hold(cc::move(hold)), _value(value) {}

private:
    impl::async_permit_hold _hold;
    T* _value = nullptr;
};

/// A reader's hold on an async_shared_mutex: the value is const, and other readers may hold it at the same time.
template <class T>
struct cc::async_shared_guard
{
    [[nodiscard]] T const& operator*() const { return *_value; }
    [[nodiscard]] T const* operator->() const { return _value; }

    async_shared_guard(async_shared_guard&&) noexcept = default;
    async_shared_guard& operator=(async_shared_guard&&) noexcept = default;

    void unlock()
    {
        _hold.release();
        _value = nullptr;
    }

    // not for callers: built by the mutex
    async_shared_guard(impl::async_permit_hold hold, T const* value) : _hold(cc::move(hold)), _value(value) {}

private:
    impl::async_permit_hold _hold;
    T const* _value = nullptr;
};

/// An async mutex that owns the value it protects, like cc::mutex — the value is reachable only through a guard.
///
///   cc::async_mutex<asset_table> assets;
///   auto const table = co_await assets.lock();   // parks on contention, never blocks the thread
///   table->insert(key, value);
///
/// Waiters are served in arrival order and the lock passes straight to the next one, so nobody barges past a queue.
/// Must outlive every guard and every pending lock; locking is not recursive.
template <class T>
struct cc::async_mutex
{
    template <class... Args>
    explicit async_mutex(Args&&... args) : _value(cc::forward<Args>(args)...)
    {
    }

    async_mutex(async_mutex const&) = delete;
    async_mutex& operator=(async_mutex const&) = delete;

    /// `co_await m.lock()` → async_mutex_guard<T>.
    [[nodiscard]] impl::async_permit_awaiter<async_mutex_guard<T>> lock()
    {
        return impl::async_permit_acquire<async_mutex_guard<T>, T>(_core, 1, &_value);
    }

    /// The lock as an async, for a raw frame: require it, then take_value() the guard out, exactly once.
    /// Dropping it untaken, before or after it is granted, gives the lock back.
    [[nodiscard]] shared_async<async_mutex_guard<T>> lock_async()
    {
        return impl::async_permit_acquire_async<async_mutex_guard<T>, T>(_core, 1, &_value);
    }

    /// The guard, if the lock is free and nobody is waiting for it.
    [[nodiscard]] cc::optional<async_mutex_guard<T>> try_lock()
    {
        return impl::async_permit_try<async_mutex_guard<T>, T>(_core, 1, &_value);
    }

private:
    impl::async_permit_core _core = impl::async_permit_core(1);
    T _value;
};

/// A reader/writer async mutex that owns its value.
///
/// Writer-preferring: once a writer waits, readers arriving after it wait behind it, and when it leaves every reader
/// queued up to the next writer is admitted together.
/// So a reader asking for a second shared lock while a writer waits deadlocks — shared locking is not recursive either.
template <class T>
struct cc::async_shared_mutex
{
    template <class... Args>
    explicit async_shared_mutex(Args&&... args) : _value(cc::forward<Args>(args)...)
    {
    }

    async_shared_mutex(async_shared_mutex const&) = delete;
    async_shared_mutex& operator=(async_shared_mutex const&) = delete;

    /// `co_await m.lock()` → the exclusive async_mutex_guard<T>.
    [[nodiscard]] impl::async_permit_awaiter<async_mutex_guard<T>> lock()
    {
        return impl::async_permit_acquire<async_mutex_guard<T>, T>(_core, writer_permits, &_value);
    }

    /// `co_await m.lock_shared()` → async_shared_guard<T>.
    [[nodiscard]] impl::async_permit_awaiter<async_shared_guard<T>> lock_shared()
    {
        return impl::async_permit_acquire<async_shared_guard<T>, T const>(_core, 1, &_value);
    }

    [[nodiscard]] shared_async<async_mutex_guard<T>> lock_async()
    {
        return impl::async_permit_acquire_async<async_mutex_guard<T>, T>(_core, writer_permits, &_value);
    }

    [[nodiscard]] shared_async<async_shared_guard<T>> lock_shared_async()
    {
        return impl::async_permit_acquire_async<async_shared_guard<T>, T const>(_core, 1, &_value);
    }

    [[nodiscard]] cc::optional<async_mutex_guard<T>> try_lock()
    {
        return impl::async_permit_try<async_mutex_guard<T>, T>(_core, writer_permits, &_value);
    }

    [[nodiscard]] cc::optional<async_shared_guard<T>> try_lock_shared()
    {
        return impl::async_permit_try<async_shared_guard<T>, T const>(_core, 1, &_value);
    }

private:
    /// A writer takes every permit and a reader one, so this bounds the readers at once and nothing else.
    static constexpr isize writer_permits = isize(1) << 40;

    impl::async_permit_core _core = impl::async_permit_core(writer_permits);
    T _value;
};

/// A hold on some permits of an async_semaphore, returned on destruction.
struct cc::async_semaphore_permit
{
    async_semaphore_permit(async_semaphore_permit&&) noexcept = default;
    async_semaphore_permit& operator=(async_semaphore_permit&&) noexcept = default;

    void release() { _hold.release(); }

    // not for callers: built by the semaphore
    explicit async_semaphore_permit(impl::async_permit_hold hold) : _hold(cc::move(hold)) {}

private:
    impl::async_permit_hold _hold;
};

/// An async counting semaphore: `permits` at once, FIFO, and a request for several waits at the head of the queue until all of them are free.
struct cc::async_semaphore
{
    explicit async_semaphore(isize permits) : _core(permits) {}

    async_semaphore(async_semaphore const&) = delete;
    async_semaphore& operator=(async_semaphore const&) = delete;

    /// `co_await s.acquire(n)` → async_semaphore_permit; `n` must be 1..size.
    [[nodiscard]] impl::async_permit_awaiter<async_semaphore_permit> acquire(isize n = 1)
    {
        CC_ASSERT(n > 0 && n <= _core.capacity(), "a semaphore request must be for 1..size permits");
        return impl::async_permit_acquire<async_semaphore_permit, void>(_core, n, nullptr);
    }

    [[nodiscard]] shared_async<async_semaphore_permit> acquire_async(isize n = 1)
    {
        CC_ASSERT(n > 0 && n <= _core.capacity(), "a semaphore request must be for 1..size permits");
        return impl::async_permit_acquire_async<async_semaphore_permit, void>(_core, n, nullptr);
    }

    [[nodiscard]] cc::optional<async_semaphore_permit> try_acquire(isize n = 1)
    {
        CC_ASSERT(n > 0 && n <= _core.capacity(), "a semaphore request must be for 1..size permits");
        return impl::async_permit_try<async_semaphore_permit, void>(_core, n, nullptr);
    }

private:
    impl::async_permit_core _core;
};
