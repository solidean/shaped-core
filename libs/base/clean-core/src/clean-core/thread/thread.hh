#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>

namespace cc
{
struct scoped_scheduler_tick;

/// Sets the calling thread's OS name (as seen in debuggers and profilers). name is UTF-8.
/// Best-effort: silently truncated where the platform is stricter (15 bytes on Linux) and a
/// no-op where thread naming is unavailable (e.g. single-threaded WebAssembly).
void set_current_thread_name(string_view name);

} // namespace cc

/// Identity of a thread, handed out on first use and never reused within a process.
/// Compare it for equality and nothing else: it is a counter, not the OS thread id a debugger or profiler shows.
enum class cc::thread_id : cc::u64
{
    invalid = 0, ///< no thread — current_thread_id() never returns it
    main = 1,    ///< whoever called mark_current_thread_as_main(); reserved, so nobody else is ever handed it
};

namespace cc
{

/// This thread's identity, claimed on first call.
[[nodiscard]] thread_id current_thread_id();

/// The OS's own id for this thread, or 0 where there is none.
///
/// Distinct from cc::thread_id, which is a portable counter and deliberately not the OS's number.
/// This is for the cases that must hand a thread to the OS or to another tool: opening a thread handle to sample it,
/// and naming a thread in a trace a profiler will read.
[[nodiscard]] u64 native_thread_id();

/// Claims thread_id::main for the calling thread, so code that must run there can check it.
/// Nothing marks it implicitly — call this from main() before anything starts a second thread.
/// The calling thread must not already hold an id, and no other thread may have claimed main.
void mark_current_thread_as_main();

/// Offers the rest of this thread's time slice to whatever else is runnable.
///
/// A SCHEDULING yield, unlike cc::spin_pause — the thread genuinely gives up the core, so this is what a wait long
/// enough to matter should call, and what a short bounded spin should not.
/// A no-op where the platform has no threads.
void this_thread_yield();

/// Blocks this thread for at least `secs`, and typically a scheduler tick longer.
/// A no-op for a non-positive duration, and where the platform has no threads.
///
/// **How much longer is a system-wide setting on Windows**, where the default scheduler tick is about 15.6 ms — so a
/// one-millisecond sleep there routinely takes fifteen.
/// cc::scoped_scheduler_tick is the lever, and cc::scheduler_tick_secs reports where it currently stands.
void this_thread_sleep_secs(double secs);

/// The system's own clock granularity in seconds, and the reason a short sleep on Windows is so much longer than it
/// was asked for: about 15.6 ms there by default, roughly a millisecond elsewhere.
///
/// **Reports the SYSTEM setting, not this process's.**
/// Since Windows 10 2004 a timeBeginPeriod request applies per process, so cc::scoped_scheduler_tick can be active and
/// this still read 15.6 ms — ask `is_active()` what was granted, not this.
[[nodiscard]] double scheduler_tick_secs();

} // namespace cc

/// Asks the OS for a finer scheduler tick for as long as this object lives.
///
/// **Process-wide on Windows and, historically, system-wide** — timeBeginPeriod affects every thread and raises power
/// draw, which is why this is a scope rather than a setting and why the docs say to hold it only while it is needed.
/// A no-op elsewhere, where the tick is already near a millisecond.
///
/// Not what a periodic task should reach for first: a waitable timer or a condition-variable deadline gets sub-tick
/// precision without changing anything for anyone else, which is what cc::rec's sampler does.
/// This is for code that must sleep in small increments and has no such option.
struct cc::scoped_scheduler_tick
{
    /// `secs` is the granularity being asked for; the OS may grant something coarser.
    explicit scoped_scheduler_tick(double secs = 0.001);
    ~scoped_scheduler_tick();

    scoped_scheduler_tick(scoped_scheduler_tick const&) = delete;
    scoped_scheduler_tick& operator=(scoped_scheduler_tick const&) = delete;

    /// Whether the request was granted; false means the sleeps around it are unchanged.
    [[nodiscard]] bool is_active() const { return _granted_ms != 0; }

private:
    unsigned int _granted_ms = 0;
};
