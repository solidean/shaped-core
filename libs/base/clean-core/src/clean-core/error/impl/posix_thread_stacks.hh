#pragma once

#include <clean-core/fwd.hh>

// Reaching threads other than the calling one, on a platform with no way to walk one from outside.
//
// **This is an initial implementation and it has not been run.**
// It is written on Windows, where none of it compiles, and its first real execution will be on Linux — so treat
// every claim below as a design statement rather than an observation, which is the opposite of how the rest of this
// subsystem is documented.
// What it needs before it can be trusted is at the bottom of this comment.
//
// **Windows suspends a thread and walks it; POSIX cannot.**
// There is no portable call that reads another thread's stack, and the two mechanisms that come close are both
// wrong: `backtrace()` captures only the caller, and attaching with ptrace from inside the process is not allowed.
// The route that does work is to make the target report ITSELF — interrupt it with a signal, have its handler
// capture its own stack into a slot prepared in advance, and collect the slots afterwards.
//
// Three properties make that safe enough for a crash handler, and each is a constraint on the code rather than a
// hope:
//
//   - **The handler allocates nothing and locks nothing.** `cc::capture_stack` is already documented as
//     allocation-free and lock-free, and the slot it writes into is reserved at install time.
//   - **The collector never waits forever.** A thread that is stuck inside a non-async-signal-safe call may never
//     run the handler, and a report that blocked on it would hang precisely where it is most wanted.
//   - **A thread that does not answer is REPORTED as unresponsive**, rather than omitted.
//     In a deadlock the thread that cannot answer is frequently the interesting one, and silence about it reads as
//     absence.
//
// **Threads are enumerated from the OS, not from a registry.**
// `/proc/self/task` lists every thread in the process, including ones clean-core never created and ones that have
// never recorded anything — which is exactly the thread a hang report is looking for.
// The cost is that this is Linux-only: macOS would need `task_threads()`, and it has no `SIGRTMIN` either.
//
// What remains before this is trustworthy, for whoever finishes it on a machine that can run it.
// These are findings from reading the code rather than a wish list; each one is a real defect or a real unknown.
//
//   1. **Nothing here has executed.** Not the handler, not the collector, not one line of the Linux arm.
//
//   2. **The collector is not async-signal-safe, and the fault path needs it to be.**
//      `opendir` and `readdir` allocate, and `cc::symbolizer` keeps an allocating cache — both are reached from
//      `report_posix_thread_stacks`, which on a SIGSEGV runs inside a signal handler.
//      The hang path is fine, since nothing there is a handler.
//      The fix is reading `/proc/self/task` with a raw `getdents64` into a fixed buffer, and either resolving
//      frames after the handler returns or printing raw addresses on the fault path.
//
//   3. **A late answer leaves the semaphore over-posted.**
//      A thread that misses its deadline still posts when it finally runs, so the NEXT `ask` can return early on
//      somebody else's post.
//      The slot state check catches it — that thread is reported unresponsive rather than given the wrong stack —
//      but the count keeps drifting, so the drift should be drained after each timeout.
//
//   4. **The signal is a guess.** `SIGRTMIN + 3` avoids the obvious collisions and nothing has checked it against
//      what else this process installs.
//
//   5. **The timeout is a guess.** One second, never measured against a genuinely stuck thread.
//
//   6. **Slots are indexed by position in the thread list**, which is only sound while one thread is asked at a
//      time.
//      That coupling is implicit and should be made explicit or removed.
//
//   7. **Untried under ThreadSanitizer**, which has opinions about a handler touching shared state.
//
//   8. **macOS is not implemented.** It has no `/proc`, no `SIGRTMIN`, and would need `task_threads()`.

namespace cc::impl
{
/// Whether this build can ask other threads to report their own stacks.
/// False everywhere but Linux today, and false there too until install_posix_thread_stack_reporter has run.
[[nodiscard]] bool posix_thread_stacks_available();

/// Installs the signal handler and reserves the per-thread slots.
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
