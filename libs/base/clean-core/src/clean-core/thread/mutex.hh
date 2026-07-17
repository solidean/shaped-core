#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/macros.hh> // CC_HAS_THREADS
#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>

#include <condition_variable>
#include <mutex>

/// Thread-safe wrapper for data T protected by a mutex
/// Rust-style mutex that encapsulates both the data and the mutex protecting it
/// Access to the protected data is only possible through scoped lock operations
///
/// Without threads (CC_HAS_THREADS == 0) the API is unchanged but there is no mutex member and no locking:
/// nothing can contend, so lock() just invokes and try_lock() always succeeds. That is 80 bytes per instance
/// on Windows, and it matters — the async pool holds one per injection queue. wait() is the exception; see it.
template <class T>
struct cc::mutex
{
    /// Acquire lock, invoke function with protected value, and return result
    /// The mutex is held for the duration of the function call
    /// Returns: The result of invoking f with the protected value (auto to prevent reference leaks)
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

    /// Attempt to acquire lock without blocking
    /// If lock is acquired, invokes function with protected value and returns result wrapped in optional
    /// If lock cannot be acquired, returns nullopt (or false for void functions) immediately without blocking
    /// Returns: optional containing the result of f, or nullopt if lock was not acquired
    ///          For void functions, returns bool indicating whether lock was acquired
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
        // Nobody to lose the race to, so the acquire cannot fail. Callers keep their has_value() branch; it is
        // simply never taken.
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

    /// Wait on condition variable with predicate, then invoke function with protected value
    /// Atomically unlocks the mutex and waits on the condition variable until the predicate returns true
    /// Once awakened and predicate is satisfied, invokes f with the protected value
    /// The mutex is held during predicate checks and the function call
    /// Returns: The result of invoking f with the protected value
    ///
    /// Without threads the predicate must ALREADY hold — only another thread could make it true, so an
    /// unsatisfied wait is a deadlock, not a slow path, and it asserts instead of hanging. This is the one
    /// operation with no honest fallback: if you need to wait for work, drive that work yourself rather than
    /// block on it (see cc::threaded_actor's unthreaded mode).
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
