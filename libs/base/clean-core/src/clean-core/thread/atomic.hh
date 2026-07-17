#pragma once

#include <clean-core/common/macros.hh> // CC_HAS_THREADS

#include <atomic>

// The atomics clean-core code uses. With threads these ARE the std types (aliases, nothing reimplemented);
// without them (CC_HAS_THREADS == 0 — WASM, or -DSC_THREADS=OFF) they are plain values with the same API,
// so a refcount bump is an `add` rather than a `lock xadd` and the orderings evaporate.
//
// That is the whole reason to go through cc:: rather than std:: directly, and why <atomic> is blessed to
// *appear* in our headers but not to be called into: a std::atomic written by hand stays atomic in a build
// that provably has no concurrency, and no flag can reach it. See
// libs/base/clean-core/docs/blessed-stdlib-headers.md.
//
// Dropping the atomicity is sound because there is no second thread to race: every operation on a plain
// value is already indivisible with respect to the only thread that exists, and the orderings constrain a
// reordering nobody can observe. This is a whole-build switch, so the two frontends never meet.
//
// Deliberately no wait()/notify(): a wait that is not already satisfied can never be satisfied on the one
// thread that could satisfy it, so the honest fallback is not a spin but a design change at the caller (see
// how sg drives its copy actors, or cc::threaded_actor's unthreaded mode). Nothing here needs it today.

namespace cc
{
// The enum and its values are the same either way — an ordering is a compile-time tag, not machinery.
using std::memory_order;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_consume;
using std::memory_order_relaxed;
using std::memory_order_release;
using std::memory_order_seq_cst;

#if CC_HAS_THREADS

template <class T>
using atomic = std::atomic<T>;

template <class T>
using atomic_ref = std::atomic_ref<T>;

using atomic_flag = std::atomic_flag;

inline void atomic_thread_fence(memory_order order) noexcept
{
    std::atomic_thread_fence(order);
}

#else

/// std::atomic's API over a plain T. Single-threaded, every op is already indivisible and the memory_order
/// arguments constrain nothing observable, so they are accepted and ignored.
template <class T>
struct atomic
{
    /// True: with no concurrency there is nothing to lock. Callers static_assert on it to reject a type
    /// the hardware would emulate with a mutex.
    static constexpr bool is_always_lock_free = true;

    constexpr atomic() noexcept = default;
    constexpr atomic(T desired) noexcept : _value(desired) {}

    atomic(atomic const&) = delete;
    atomic& operator=(atomic const&) = delete;

    T operator=(T desired) noexcept
    {
        _value = desired;
        return desired;
    }
    operator T() const noexcept { return _value; }

    void store(T desired, memory_order = memory_order_seq_cst) noexcept { _value = desired; }
    [[nodiscard]] T load(memory_order = memory_order_seq_cst) const noexcept { return _value; }

    T exchange(T desired, memory_order = memory_order_seq_cst) noexcept
    {
        T const old = _value;
        _value = desired;
        return old;
    }

    bool compare_exchange_weak(T& expected, T desired, memory_order, memory_order) noexcept
    {
        return _compare_exchange(expected, desired);
    }
    bool compare_exchange_weak(T& expected, T desired, memory_order = memory_order_seq_cst) noexcept
    {
        return _compare_exchange(expected, desired);
    }
    bool compare_exchange_strong(T& expected, T desired, memory_order, memory_order) noexcept
    {
        return _compare_exchange(expected, desired);
    }
    bool compare_exchange_strong(T& expected, T desired, memory_order = memory_order_seq_cst) noexcept
    {
        return _compare_exchange(expected, desired);
    }

    // Instantiated only where called, so a pointer/bool atomic that never fetches stays well-formed.
    T fetch_add(T arg, memory_order = memory_order_seq_cst) noexcept
    {
        return _fetch([&](T v) { return T(v + arg); });
    }
    T fetch_sub(T arg, memory_order = memory_order_seq_cst) noexcept
    {
        return _fetch([&](T v) { return T(v - arg); });
    }
    T fetch_and(T arg, memory_order = memory_order_seq_cst) noexcept
    {
        return _fetch([&](T v) { return T(v & arg); });
    }
    T fetch_or(T arg, memory_order = memory_order_seq_cst) noexcept
    {
        return _fetch([&](T v) { return T(v | arg); });
    }
    T fetch_xor(T arg, memory_order = memory_order_seq_cst) noexcept
    {
        return _fetch([&](T v) { return T(v ^ arg); });
    }

    // std::atomic's operators return the NEW value, unlike fetch_*; ++x and x++ differ accordingly.
    T operator++() noexcept { return T(fetch_add(T(1)) + T(1)); }
    T operator++(int) noexcept { return fetch_add(T(1)); }
    T operator--() noexcept { return T(fetch_sub(T(1)) - T(1)); }
    T operator--(int) noexcept { return fetch_sub(T(1)); }
    T operator+=(T arg) noexcept { return T(fetch_add(arg) + arg); }
    T operator-=(T arg) noexcept { return T(fetch_sub(arg) - arg); }
    T operator&=(T arg) noexcept { return T(fetch_and(arg) & arg); }
    T operator|=(T arg) noexcept { return T(fetch_or(arg) | arg); }
    T operator^=(T arg) noexcept { return T(fetch_xor(arg) ^ arg); }

private:
    bool _compare_exchange(T& expected, T desired) noexcept
    {
        // Never fails spuriously: the weak/strong distinction exists for LL/SC hardware, and there is no
        // contention to lose to. A caller's retry loop simply exits on the first pass.
        if (_value != expected)
        {
            expected = _value;
            return false;
        }
        _value = desired;
        return true;
    }

    template <class F>
    T _fetch(F&& op) noexcept
    {
        T const old = _value;
        _value = op(old);
        return old;
    }

    T _value{};
};

/// std::atomic_ref's API over a plain lvalue. Same reasoning as cc::atomic; the referent stays a plain T.
template <class T>
struct atomic_ref
{
    static constexpr bool is_always_lock_free = true;

    explicit atomic_ref(T& obj) noexcept : _value(&obj) {}

    void store(T desired, memory_order = memory_order_seq_cst) const noexcept { *_value = desired; }
    [[nodiscard]] T load(memory_order = memory_order_seq_cst) const noexcept { return *_value; }

    T exchange(T desired, memory_order = memory_order_seq_cst) const noexcept
    {
        T const old = *_value;
        *_value = desired;
        return old;
    }

    T fetch_add(T arg, memory_order = memory_order_seq_cst) const noexcept
    {
        return _fetch([&](T v) { return T(v + arg); });
    }
    T fetch_sub(T arg, memory_order = memory_order_seq_cst) const noexcept
    {
        return _fetch([&](T v) { return T(v - arg); });
    }
    T fetch_and(T arg, memory_order = memory_order_seq_cst) const noexcept
    {
        return _fetch([&](T v) { return T(v & arg); });
    }
    T fetch_or(T arg, memory_order = memory_order_seq_cst) const noexcept
    {
        return _fetch([&](T v) { return T(v | arg); });
    }
    T fetch_xor(T arg, memory_order = memory_order_seq_cst) const noexcept
    {
        return _fetch([&](T v) { return T(v ^ arg); });
    }

    // Return the NEW value, unlike fetch_*; ++x and x++ differ accordingly.
    T operator++() const noexcept { return T(fetch_add(T(1)) + T(1)); }
    T operator++(int) const noexcept { return fetch_add(T(1)); }
    T operator--() const noexcept { return T(fetch_sub(T(1)) - T(1)); }
    T operator--(int) const noexcept { return fetch_sub(T(1)); }
    T operator+=(T arg) const noexcept { return T(fetch_add(arg) + arg); }
    T operator-=(T arg) const noexcept { return T(fetch_sub(arg) - arg); }
    T operator&=(T arg) const noexcept { return T(fetch_and(arg) & arg); }
    T operator|=(T arg) const noexcept { return T(fetch_or(arg) | arg); }
    T operator^=(T arg) const noexcept { return T(fetch_xor(arg) ^ arg); }

    T operator=(T desired) const noexcept
    {
        store(desired);
        return desired;
    }
    operator T() const noexcept { return load(); }

private:
    template <class F>
    T _fetch(F&& op) const noexcept
    {
        T const old = *_value;
        *_value = op(old);
        return old;
    }

    T* _value;
};

/// std::atomic_flag's API over a plain bool. Constinit-able and trivially destructible like the real one —
/// load-bearing for the static spinlocks that must outlive static destruction (see node_allocation.cc).
struct atomic_flag
{
    constexpr atomic_flag() noexcept = default;

    bool test_and_set(memory_order = memory_order_seq_cst) noexcept
    {
        bool const old = _value;
        _value = true;
        return old;
    }
    void clear(memory_order = memory_order_seq_cst) noexcept { _value = false; }
    [[nodiscard]] bool test(memory_order = memory_order_seq_cst) const noexcept { return _value; }

private:
    bool _value = false;
};

/// No-op: a fence orders one thread's writes against another's reads, and there is no other thread.
inline void atomic_thread_fence(memory_order) noexcept
{
}

#endif

// =========================================================================================================
// Read-modify-write on a plain lvalue
// =========================================================================================================
// For memory the surrounding code owns as a plain value (a header word, a bitmap) and only occasionally
// touches atomically. seq_cst: these are for correctness-first call sites, not tuned hot paths — reach for
// cc::atomic_ref directly when an ordering matters.

/// Atomically adds and returns the old value.
/// Usage: int counter = 0; int old = cc::atomic_add(counter, 1); // counter 1, old 0
template <class T>
T atomic_add(T& v, T rhs) noexcept
{
    return cc::atomic_ref<T>(v).fetch_add(rhs);
}

/// Atomically subtracts and returns the old value.
template <class T>
T atomic_sub(T& v, T rhs) noexcept
{
    return cc::atomic_ref<T>(v).fetch_sub(rhs);
}

/// Atomically ANDs and returns the old value.
template <class T>
T atomic_and(T& v, T rhs) noexcept
{
    return cc::atomic_ref<T>(v).fetch_and(rhs);
}

/// Atomically ORs and returns the old value.
template <class T>
T atomic_or(T& v, T rhs) noexcept
{
    return cc::atomic_ref<T>(v).fetch_or(rhs);
}

/// Atomically XORs and returns the old value.
template <class T>
T atomic_xor(T& v, T rhs) noexcept
{
    return cc::atomic_ref<T>(v).fetch_xor(rhs);
}
} // namespace cc
