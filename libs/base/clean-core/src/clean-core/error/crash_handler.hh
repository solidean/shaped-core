#pragma once

#include <clean-core/string/string_view.hh>

// Sanitizers (ASan/TSan/MSan) install their own fault handlers and print far richer diagnostics, so overriding them would suppress those reports.
// So under an active sanitizer, installation is a no-op and the runtime's handlers stay in place; this says whether that is the case.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define CC_CRASH_HANDLER_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define CC_CRASH_HANDLER_SANITIZED 1
#endif
#endif
#ifndef CC_CRASH_HANDLER_SANITIZED
#define CC_CRASH_HANDLER_SANITIZED 0
#endif

namespace cc
{
struct crash_report_files;
} // namespace cc

/// Where crash reports are kept besides stderr, so a crash can be read after the fact: see set_crash_report_files.
struct cc::crash_report_files
{
    /// Created if missing; empty keeps reports on stderr alone.
    cc::string_view directory;

    /// What each file is named for, the executable's name say: a report is `<name>-<date>-<time>-<pid>.txt`.
    cc::string_view name = "crash";

    /// Windows: a minidump beside the report, `.dmp`: every thread's stack and the memory its pointers reach.
    /// Readable only with the binary and PDB that wrote it, so whoever keeps the dump keeps those too.
    bool minidump = true;
};

namespace cc
{
/// Signature of a crash-context hook (see add_crash_context_hook).
using crash_context_hook = void (*)() noexcept;

/// Installs a process-wide handler for fatal, non-recoverable faults: segmentation faults, illegal instructions, FP exceptions, bus errors, breakpoints, and abort()/terminate().
/// On such a fault the handler writes a short description, any registered context, the faulting thread's stacktrace, and — on Windows and Linux — every other thread's.
/// It then lets the process terminate with the fault's normal disposition.
///
/// The other threads are the point of a deadlock or a hung test, where the thread that noticed is never the one that matters.
/// Reaching them means suspending each in turn and walking it with DbgHelp on Windows, and asking each to unwind itself through a signal on Linux.
/// Elsewhere only the faulting thread is reported, and a core dump carries the rest.
///
/// On Windows a stack overflow is reported by a thread parked for it at install, since the overflowed stack has no room left for the report.
/// It walks the faulting thread from the fault's context: the top frames, where the recursion shows, the bottom ones and the depth.
///
/// Idempotent — installing more than once is harmless.
/// Implemented per platform (Windows SEH plus SIGABRT, POSIX signals); a best-effort no-op where unsupported.
///
/// The handler runs in a constrained context — a signal handler or SEH filter — and is not fully async-signal-safe.
/// It is a developer diagnostic, not a production fault-recovery mechanism.
void install_crash_handler();

/// Keeps every report from here on in a file of its own too, and on Windows a minidump beside it.
///
/// **Everything a report writes to stderr goes to its file**, the context hooks included: stderr is pointed at the file while the report is written, then the file is copied back to stderr.
/// A report from report_all_thread_stacks is kept the same way.
/// The directory is created and the path formatted now, so the fault path only adds the time; a later call replaces the setting.
/// A no-op under a sanitizer, whose own reports stay on stderr.
void set_crash_report_files(crash_report_files const& files);

/// The last report kept in a file, its path without the extension: `.txt` is the report, `.dmp` its minidump if one was written.
/// Empty when no report was kept in a file.
[[nodiscard]] cc::string_view last_crash_report_path();

/// Write this thread's stacktrace and every other thread's, now, with nothing having crashed.
///
/// **It exists separately from `cc::stacktrace` for the other threads**:
/// in a deadlock or a wait that never finished, the thread that noticed is never the one that matters.
/// Reaching them means suspending and walking each on Windows, and signalling each to unwind itself on Linux, neither
/// of which `std::stacktrace` can do -- so the other threads are reported on those two, and the calling one everywhere.
/// Every thread's open recording scopes follow the stacks, on every platform.
///
/// Writes to stderr, runs every registered crash-context hook, and interrupts the other threads while it reads them.
/// A diagnostic for a program that has already gone wrong -- a test whose wait budget expired, a watchdog that
/// fired -- and not something to call on a path that works.
void report_all_thread_stacks(char const* reason) noexcept;

/// Registers a hook called from within the crash handler, before the stacktrace, to print extra context — the currently running test, say.
/// Keep it minimal and allocation-free; it must not throw.
/// Hooks run in registration order, and excess hooks past a small fixed capacity are ignored.
/// Not thread-safe against concurrent installation.
void add_crash_context_hook(crash_context_hook hook);
} // namespace cc
