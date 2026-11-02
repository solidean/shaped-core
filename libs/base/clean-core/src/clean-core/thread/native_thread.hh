#pragma once

#include <clean-core/function/unique_function.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>

// cc::native_thread: an OS thread whose stack size we choose, the same on every platform.
//
// Work does not start threads: it goes to cc::async_thread_pool, or to a cc::threaded_actor when it needs a thread of its own.
// This is the escape hatch beneath those, for what must own an OS thread for its whole life — the pool's workers, an actor, a watchdog.
// It replaces std::thread, whose stack is the platform's default: 8 MiB on Linux, 1 MiB on Windows, 512 KiB for a secondary thread on macOS.
// A recursion that fits on Linux then overflows on macOS, and only there.

/// How a cc::native_thread starts.
struct cc::native_thread_config
{
    /// The OS name, set on the new thread before its body runs (cc::set_current_thread_name); empty leaves it unnamed.
    string_view name = {};

    /// The stack the thread gets, rounded up to what the platform accepts; 0 takes the platform's default.
    /// Address space is reserved, not committed, so 8 MiB costs a 64-bit process nothing it does not touch.
    /// Emscripten is the exception: a pthread's stack is carved from linear memory, so it keeps Emscripten's default.
    isize stack_bytes = default_stack_bytes;

#ifdef __EMSCRIPTEN__
    static constexpr isize default_stack_bytes = 0;
#else
    static constexpr isize default_stack_bytes = isize(8) << 20;
#endif
};

/// An OS thread running one body, started with cc::native_thread::start.
///
/// Move-only, and must be joined before it is destroyed or assigned over, as std::thread must.
/// There is no detach: a thread nobody joins outlives the state it was given.
/// An exception leaving the body terminates the process.
///
/// Without threads (CC_HAS_THREADS == 0) start runs the body to its end on the calling thread, and join returns at once.
/// So a body that waits for another thread never returns there; a caller that has one checks CC_HAS_THREADS, as every caller does today.
struct cc::native_thread
{
    [[nodiscard]] static native_thread start(cc::unique_function<void()> body, native_thread_config const& config = {});

    /// Blocks until the body has returned; the thread must be joinable.
    void join();

    /// True from start until join; false for a default-constructed or moved-from thread.
    [[nodiscard]] bool joinable() const { return _start.is_valid(); }

    native_thread() = default;
    native_thread(native_thread&& rhs) noexcept = default;
    native_thread& operator=(native_thread&& rhs) noexcept;
    native_thread(native_thread const&) = delete;
    native_thread& operator=(native_thread const&) = delete;
    ~native_thread();

private:
    /// What the new thread reads, at an address that stays put while the cc::native_thread moves.
    struct start_data
    {
        cc::unique_function<void()> body;
        cc::string name;
    };

    cc::unique_ptr<start_data> _start;
    u64 _handle = 0; // a pthread_t or a Windows HANDLE, kept as an integer so the OS headers stay out of this one
};
