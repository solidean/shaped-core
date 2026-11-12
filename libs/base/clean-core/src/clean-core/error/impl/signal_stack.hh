#pragma once

// A stack of its own for the crash handler's signals, per thread, on Linux and macOS.
//
// **A fault from an overflowed stack has no stack left to run its handler on**, so without one the process dies with
// an empty report.
// `sigaltstack` gives the handler memory of its own, and the handlers are installed `SA_ONSTACK` to run there.
//
// **Only threads that took one are covered**: the one that installs the crash handler, and every `cc::native_thread`,
// which takes one at its start.
// A thread clean-core did not create, such as a dispatch queue's, still dies without a report when it overflows.
//
// A thread records its own stack's bounds when it takes the stack, so the handler can name an overflow without calling
// anything that is not async-signal-safe.

namespace cc::impl
{
/// Lets threads take signal stacks from now on, and gives the calling thread one.
/// Called by cc::install_crash_handler; nothing before it takes a stack.
void enable_thread_signal_stacks() noexcept;

/// Gives the calling thread its signal stack and records its own stack's bounds.
/// Does nothing before enable_thread_signal_stacks, on a platform without sigaltstack, or when the thread has one.
void enter_thread_signal_stack() noexcept;

/// Removes and frees what enter_thread_signal_stack gave the calling thread; nothing where it gave nothing.
void leave_thread_signal_stack() noexcept;

/// Whether `address` lies at or just below the calling thread's own stack, which is where an overflow faults.
/// False on a thread that took no signal stack, whose bounds are unknown.
/// Async-signal-safe.
[[nodiscard]] bool is_stack_overflow_address(void const* address) noexcept;
} // namespace cc::impl
