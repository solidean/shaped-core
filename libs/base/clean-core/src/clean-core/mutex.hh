#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/utility.hh>

#include <condition_variable>
#include <mutex>

/// Thread-safe wrapper for data T protected by a mutex
/// Rust-style mutex that encapsulates both the data and the mutex protecting it
/// Access to the protected data is only possible through scoped lock operations
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
        std::lock_guard lock(_mutex);
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
    }

    /// Wait on condition variable with predicate, then invoke function with protected value
    /// Atomically unlocks the mutex and waits on the condition variable until the predicate returns true
    /// Once awakened and predicate is satisfied, invokes f with the protected value
    /// The mutex is held during predicate checks and the function call
    /// Returns: The result of invoking f with the protected value
    /// Usage:
    ///   cc::mutex<int> counter;
    ///   std::condition_variable cv;
    ///   counter.wait(cv, [](int const& val) { return val > 0; }, [](int& val) { val--; });
    template <class Pred, class F>
    auto wait(std::condition_variable& cv, Pred&& pred, F&& f)
    {
        std::unique_lock lock(_mutex);
        cv.wait(lock, [&]() { return cc::invoke(pred, _value); });
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
    std::mutex _mutex;
};
