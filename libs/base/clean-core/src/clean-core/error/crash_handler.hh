#pragma once

namespace cc
{
/// Signature of a crash-context hook (see add_crash_context_hook).
using crash_context_hook = void (*)() noexcept;

/// Installs a process-wide handler for fatal, non-recoverable faults: segmentation faults,
/// illegal instructions, FP exceptions, bus errors, breakpoints, and abort()/terminate().
/// On such a fault the handler writes a short description, any registered context, and a
/// stacktrace to stderr, then lets the process terminate with the fault's normal disposition.
///
/// Idempotent — installing more than once is harmless. Implemented per platform (Windows SEH
/// plus SIGABRT, POSIX signals); a best-effort no-op where unsupported.
///
/// The handler runs in a constrained context (a signal handler / SEH filter) and is not fully
/// async-signal-safe: it is a developer diagnostic, not a production fault-recovery mechanism.
void install_crash_handler();

/// Registers a hook called from within the crash handler, before the stacktrace, to print extra
/// context (e.g. the currently running test). Keep it minimal and allocation-free; it must not
/// throw. Hooks run in registration order. Not thread-safe against concurrent installation.
/// Excess hooks past a small fixed capacity are ignored.
void add_crash_context_hook(crash_context_hook hook);
} // namespace cc
