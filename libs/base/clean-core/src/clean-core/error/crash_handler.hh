#pragma once

namespace cc
{
/// Signature of a crash-context hook (see add_crash_context_hook).
using crash_context_hook = void (*)() noexcept;

/// Installs a process-wide handler for fatal, non-recoverable faults: segmentation faults, illegal instructions, FP exceptions, bus errors, breakpoints, and abort()/terminate().
/// On such a fault the handler writes a short description, any registered context, the faulting thread's stacktrace, and — on Windows — every other thread's.
/// It then lets the process terminate with the fault's normal disposition.
///
/// The other threads are the point of a deadlock or a hung test, where the thread that noticed is never the one that matters.
/// Reaching them means suspending each in turn and walking it with DbgHelp, which std::stacktrace cannot do.
/// Elsewhere only the faulting thread is reported, and a core dump carries the rest.
///
/// Idempotent — installing more than once is harmless.
/// Implemented per platform (Windows SEH plus SIGABRT, POSIX signals); a best-effort no-op where unsupported.
///
/// The handler runs in a constrained context — a signal handler or SEH filter — and is not fully async-signal-safe.
/// It is a developer diagnostic, not a production fault-recovery mechanism.
void install_crash_handler();

/// Registers a hook called from within the crash handler, before the stacktrace, to print extra context — the currently running test, say.
/// Keep it minimal and allocation-free; it must not throw.
/// Hooks run in registration order, and excess hooks past a small fixed capacity are ignored.
/// Not thread-safe against concurrent installation.
void add_crash_context_hook(crash_context_hook hook);
} // namespace cc
