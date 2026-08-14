#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>

// COST NOTE: the STL headers below reach MSVC's <xutility>, which pulls <immintrin.h>.
// That is the whole AVX-512 intrinsic surface — ~43 extra files, and most of this header's parse time.
// <memory>, <string>, <string_view>, <mutex>, <system_error>, <ranges> and <chrono> all reach it.
// <type_traits>, <utility> and <atomic> do not, and are cheap by comparison.
// So keeping one of the first group out of a widely-included header is worth real time.
// docs/notes/build-times.md has the measurement and the per-header table.
#include <condition_variable>
#include <mutex>

/// Rust-style mutex: it owns both the data and the lock protecting it, so the value is reachable only through a scoped lock operation.
///
/// Without threads (CC_HAS_THREADS == 0) the API is unchanged, but there is no mutex member and no locking.
/// Nothing can contend, so lock() just invokes and try_lock() always succeeds.
/// That saves 80 bytes per instance on Windows, and it matters — the async pool holds one per injection queue.
/// wait() is the exception; see it.
template <class T>
struct cc::mutex
{
    /// Acquire the lock, invoke `f` with the protected value, and return its result.
    /// The lock is held for the duration of the call, and the `auto` return is what keeps a reference to the value from leaking out.
    /// Usage:
    ///   cc::mutex<int> counter;
    ///   counter.lock([](int& val) { val++; });
    ///   int current = counter.lock([](int const& val) { return val; });
    template <class F>
    auto lock(F&& f)
    {
#if CC_HAS_THREADS
        std::lock_guard lock(_mutex);
#endif
        return cc::invoke(cc::forward<F>(f), _value);
    }

    /// Acquire the lock without blocking, and on success invoke `f` with the protected value.
    /// Returns its result in an optional, nullopt if the lock was not acquired — or, for a void `f`, a plain bool saying whether it was.
    /// Usage:
    ///   cc::mutex<int> counter;
    ///   if (auto result = counter.try_lock([](int& val) { return val++; }); result.has_value())
    ///       // lock was acquired and result is available
    ///   if (counter.try_lock([](int& val) { val++; }))
    ///       // lock was acquired (void function, returns bool)
    template <class F>
    auto try_lock(F&& f)
    {
        using result_t = decltype(cc::invoke(cc::forward<F>(f), _value));
#if !CC_HAS_THREADS
        // Nobody to lose the race to, so the acquire cannot fail.
        // Callers keep their has_value() branch; it is simply never taken.
        if constexpr (std::is_void_v<result_t>)
        {
            cc::invoke(cc::forward<F>(f), _value);
            return true;
        }
        else
        {
            return optional<result_t>(cc::invoke(cc::forward<F>(f), _value));
        }
#else
        std::unique_lock lock(_mutex, std::try_to_lock);

        if constexpr (std::is_void_v<result_t>)
        {
            if (lock.owns_lock())
            {
                cc::invoke(cc::forward<F>(f), _value);
                return true;
            }
            else
            {
                return false;
            }
        }
        else
        {
            if (lock.owns_lock())
            {
                return optional<result_t>(cc::invoke(cc::forward<F>(f), _value));
            }
            else
            {
                return optional<result_t>();
            }
        }
#endif
    }

    /// Scoped alternative to lock(f) — reach for lock(f) first.
    /// This is for the critical section that cannot be one call: it spans the caller's own statements, or the lock is handed back to a caller.
    /// The returned guard holds the lock until it dies and reaches the value through -> and *.
    /// Usage:
    ///   cc::mutex<cc::vector<int>> m;
    ///   auto values = m.lock_scoped();
    ///   values->push_back(1);
    [[nodiscard]] mutex_guard<T> lock_scoped()
    {
#if CC_HAS_THREADS
        return mutex_guard<T>(_value, _mutex);
#else
        return mutex_guard<T>(_value);
#endif
    }

    /// Atomically unlock and wait on `cv` until `pred` holds, then invoke `f` with the protected value.
    /// The mutex is held during every predicate check and during the call.
    ///
    /// Without threads the predicate must ALREADY hold — only another thread could make it true.
    /// An unsatisfied wait is then a deadlock rather than a slow path, and it asserts instead of hanging.
    /// This is the one operation with no honest fallback: to wait for work, drive that work yourself rather than block on it (see cc::threaded_actor's unthreaded mode).
    /// Usage:
    ///   cc::mutex<int> counter;
    ///   std::condition_variable cv;
    ///   counter.wait(cv, [](int const& val) { return val > 0; }, [](int& val) { val--; });
    template <class Pred, class F>
    auto wait(std::condition_variable& cv, Pred&& pred, F&& f)
    {
#if CC_HAS_THREADS
        std::unique_lock lock(_mutex);
        cv.wait(lock, [&]() { return cc::invoke(pred, _value); });
#else
        (void)cv;
        CC_ASSERT(cc::invoke(pred, _value), "waiting on a predicate no other thread can ever make true");
#endif
        return cc::invoke(cc::forward<F>(f), _value);
    }

    /// Default constructor - default-constructs the protected value
    mutex() = default;

    /// Construct with initial value (copy)
    explicit mutex(T const& value) : _value(value)
    {
        static_assert(std::is_copy_constructible_v<T>, "T must be copy constructible");
    }

    /// Construct with initial value (move)
    explicit mutex(T&& value) : _value(cc::move(value))
    {
        static_assert(std::is_move_constructible_v<T>, "T must be move constructible");
    }

    /// Construct with initial value (in-place construction)
    template <class... Args>
    explicit mutex(Args&&... args) : _value(cc::forward<Args>(args)...)
    {
    }

private:
    T _value;
#if CC_HAS_THREADS
    std::mutex _mutex;
#endif
};

/// What mutex::lock_scoped returns: a hold on the lock, reaching the guarded value through -> and *.
/// The lock is released when the guard dies.
///
/// Not the default way to reach a mutex's value — lock(f) is, and it keeps every reference to the value inside the callback.
/// A guard is the exception, for the critical section that cannot be one call.
/// The reference lives exactly as long as the guard, so the guard is move-only and must not outlive its mutex.
///
/// Without threads there is no lock to hold, only the pointer — the same shape cc::mutex itself takes.
template <class T>
class cc::mutex_guard
{
public:
    [[nodiscard]] T& operator*() const { return *_value; }
    [[nodiscard]] T* operator->() const { return _value; }

    mutex_guard(mutex_guard&&) = default;
    mutex_guard& operator=(mutex_guard&&) = default;

    // Deleted explicitly rather than left to the lock member: without threads there is none, and the implicit copy would come back.
    mutex_guard(mutex_guard const&) = delete;
    mutex_guard& operator=(mutex_guard const&) = delete;

private:
    friend struct cc::mutex<T>;

    // Both forms are explicit: an explicitness that changed with CC_HAS_THREADS would make a call site build in one threading mode and not the other.
#if CC_HAS_THREADS
    explicit mutex_guard(T& value, std::mutex& m) : _value(&value), _lock(m) {}
#else
    explicit mutex_guard(T& value) : _value(&value) {}
#endif

    T* _value;
#if CC_HAS_THREADS
    std::unique_lock<std::mutex> _lock;
#endif
};
