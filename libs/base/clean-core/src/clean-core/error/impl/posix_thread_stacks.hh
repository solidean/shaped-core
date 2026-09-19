#pragma once

#include <clean-core/fwd.hh>

// Reaching threads other than the calling one, on a platform with no way to walk one from outside.
//
// **Windows suspends a thread and walks it; POSIX cannot.**
// There is no portable call that reads another thread's stack, and the two mechanisms that come close are both
// wrong: `backtrace()` captures only the caller, and attaching with ptrace from inside the process is not allowed.
// The route that does work is to make the target report ITSELF — interrupt it with a signal, have its handler
// unwind its own stack into a slot prepared in advance, and collect the slot afterwards.
//
// **The handler unwinds from tables, not frame pointers.**
// An asked thread is nearly always parked in the C library, which keeps no frame chain, so a frame-pointer chase
// from inside the handler stops at the first link and reports nothing — measured, every thread came back empty.
// `_Unwind_Backtrace` walks through the signal trampoline into the interrupted code instead.
//
// Three properties make that safe enough for a crash handler, and each is a constraint on the code rather than a
// hope:
//
//   - **Nothing on either side allocates.** The thread list is read with raw `openat` and `getdents64` into a
//     stack buffer, frames are printed as raw addresses rather than through the symbolizer's cache, and the slot is
//     reserved at install time.
//   - **The collector never waits forever.** A thread stuck inside a non-async-signal-safe call may never run the
//     handler, and a report that blocked on it would hang precisely where it is most wanted.
//   - **A thread that does not answer is REPORTED as unresponsive**, rather than omitted.
//     In a deadlock the thread that cannot answer is frequently the interesting one, and silence about it reads as
//     absence.
//
// **Threads are enumerated from the OS, not from a registry.**
// `/proc/self/task` lists every thread in the process, including ones clean-core never created and ones that have
// never recorded anything — which is exactly the thread a hang report is looking for.
// The cost is that this is Linux-only: macOS would need `task_threads()`, and it has no `SIGRTMIN` either.
//
// What is still open:
//
//   1. **The unwinder is not async-signal-safe on paper.** It finds unwind tables through `dl_iterate_phdr`, which
//      takes the loader lock, so a thread interrupted inside `dlopen` cannot answer and is reported unresponsive.
//      Warming it at install removes the lazy setup, not that lock.
//   2. **A late answer can race the next request.** A thread that passed its tid check just before its request
//      timed out may still be writing when the next thread is asked, and its frames would be reported under that
//      thread's name.
//      The window is one timeout wide and needs a thread to take a full second to unwind.
//   3. **The signal and the timeout are choices, not measurements.** `SIGRTMIN + 3` avoids the obvious collisions
//      and nothing has checked it against what else a process installs; one second has not been tested against a
//      genuinely stuck thread.
//   4. **Addresses are absolute.** Resolving them offline needs the module bases, which the recording carries.
//   5. **Untried under ThreadSanitizer**, which has opinions about a handler touching shared state.
//   6. **macOS is not implemented.** It has no `/proc`, no `SIGRTMIN`, and would need `task_threads()`.

namespace cc::impl
{
/// Whether this build can ask other threads to report their own stacks.
/// False everywhere but Linux today, and false there too until install_posix_thread_stack_reporter has run.
[[nodiscard]] bool posix_thread_stacks_available();

/// Installs the signal handler, reserves the slot, and warms the unwinder.
///
/// Called from cc::install_crash_handler, and idempotent.
/// Reserving now is the point: the collector runs when the process is already in trouble, and neither it nor the
/// handler may allocate.
void install_posix_thread_stack_reporter();

/// Asks every thread but the caller to report its own stack, and writes what came back to stderr.
///
/// Returns false when nothing could be asked at all — no handler installed, or the thread list unreadable — so the
/// caller can say so rather than printing an empty section.
[[nodiscard]] bool report_posix_thread_stacks() noexcept;
} // namespace cc::impl
