#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/string/string_view.hh>

namespace cc
{
/// Sets the calling thread's OS name (as seen in debuggers and profilers). name is UTF-8.
/// Best-effort: silently truncated where the platform is stricter (15 bytes on Linux) and a
/// no-op where thread naming is unavailable (e.g. single-threaded WebAssembly).
void set_current_thread_name(string_view name);

/// Number of hardware threads the OS reports (std::thread::hardware_concurrency), clamped to at least 1.
/// A "don't know" answer from the platform (0) becomes 1; returns 1 where threads are unavailable.
[[nodiscard]] int num_hardware_threads();

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

/// Claims thread_id::main for the calling thread, so code that must run there can check it.
/// Nothing marks it implicitly — call this from main() before anything starts a second thread.
/// The calling thread must not already hold an id, and no other thread may have claimed main.
void mark_current_thread_as_main();
} // namespace cc
