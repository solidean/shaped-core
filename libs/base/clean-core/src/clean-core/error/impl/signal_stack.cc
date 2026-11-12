#include "signal_stack.hh"

#include <clean-core/error/crash_handler.hh> // CC_CRASH_HANDLER_SANITIZED

#if (defined(__linux__) || defined(__APPLE__)) && !defined(__EMSCRIPTEN__) && !CC_CRASH_HANDLER_SANITIZED

#include <clean-core/thread/atomic.hh>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>

namespace
{
/// Enough for the report the handler writes on it: stdio, a stack walk, and the walk's symbols.
constexpr size_t k_signal_stack_bytes = size_t(256) << 10;

/// How far below its stack's lowest byte an overflow may fault: a frame larger than the guard page skips past it.
constexpr uintptr_t k_overflow_reach = uintptr_t(1) << 20;

/// How far above it: the guard can sit inside the reported range, and macOS's pages are 16 KiB.
constexpr uintptr_t k_guard_reach = uintptr_t(64) << 10;

cc::atomic<bool> g_enabled = false;

/// Trivial, so a handler reading it on a thread that never took a stack finds zeros rather than running an initializer.
struct thread_signal_stack
{
    void* memory = nullptr;
    uintptr_t stack_low = 0;
};

thread_local thread_signal_stack t_stack;

/// The lowest address of the calling thread's own stack, or 0 where the platform cannot say.
[[nodiscard]] uintptr_t own_stack_low() noexcept
{
#if defined(__APPLE__)
    auto const self = pthread_self();
    return uintptr_t(pthread_get_stackaddr_np(self)) - uintptr_t(pthread_get_stacksize_np(self));
#else
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0)
        return 0;
    void* low = nullptr;
    size_t size = 0;
    auto const found = pthread_attr_getstack(&attributes, &low, &size) == 0;
    pthread_attr_destroy(&attributes);
    return found ? uintptr_t(low) : 0;
#endif
}
} // namespace

void cc::impl::enable_thread_signal_stacks() noexcept
{
    g_enabled.store(true, std::memory_order_release);
    enter_thread_signal_stack();
}

void cc::impl::enter_thread_signal_stack() noexcept
{
    if (!g_enabled.load(std::memory_order_acquire) || t_stack.memory != nullptr)
        return;

    auto* const memory = mmap(nullptr, k_signal_stack_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED)
        return;

    auto stack = stack_t{};
    stack.ss_sp = memory;
    stack.ss_size = k_signal_stack_bytes;
    if (sigaltstack(&stack, nullptr) != 0)
    {
        munmap(memory, k_signal_stack_bytes);
        return;
    }
    t_stack.memory = memory;
    t_stack.stack_low = own_stack_low();
}

void cc::impl::leave_thread_signal_stack() noexcept
{
    if (t_stack.memory == nullptr)
        return;

    auto stack = stack_t{};
    stack.ss_flags = SS_DISABLE;
    (void)sigaltstack(&stack, nullptr);
    munmap(t_stack.memory, k_signal_stack_bytes);
    t_stack = {};
}

bool cc::impl::is_stack_overflow_address(void const* address) noexcept
{
    auto const low = t_stack.stack_low;
    auto const at = uintptr_t(address);
    return low != 0 && at + k_overflow_reach >= low && at < low + k_guard_reach;
}

#else

void cc::impl::enable_thread_signal_stacks() noexcept
{
}
void cc::impl::enter_thread_signal_stack() noexcept
{
}
void cc::impl::leave_thread_signal_stack() noexcept
{
}
bool cc::impl::is_stack_overflow_address(void const*) noexcept
{
    return false;
}

#endif
