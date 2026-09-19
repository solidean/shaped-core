#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/function/function_ref.hh>
#include <clean-core/record/fwd.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/thread.hh>

// What every thread currently has open, read from outside without stopping any of them.
//
// **This is the per-thread report a machine stack cannot give, and on one platform it is the only one there is.**
// A stack says where the program counter is; a scope stack says which logical task a thread is inside — `frame 918 >
// upload_pass > await buffer_ready` — which in a deadlock is usually what identifies the bug.
//
// It is also the only per-thread answer available on wasm.
// A pthread there is a Web Worker, a blocked worker processes no messages, and there is no interrupt: a wedged worker
// cannot be made to report itself, and nothing else can walk it.
// The events it already published, on the other hand, are sitting in shared memory.
//
// **Derived from the recording rather than from another thread's thread-local state**, which is what makes it
// race-free: a chunk's committed watermark is release-stored after the bytes it covers, so a reader is always looking
// at whole events, and the writer is never disturbed.
// Reaching into `writer_tls` would be cheaper and would be a data race on the single most performance-sensitive
// object in the system.
//
// What it costs is completeness, and the shape of the loss is worth knowing: a scope opened inside the window is
// known by name, and one opened long before it is known only if the chunk preamble still names it — which is the
// outermost `named_scope_capacity` of them, and exactly the long-lived frame or worker scopes a reader wants.
// A level in between reports as open with no name rather than as absent.

namespace cc::rec
{
struct thread_scope_view;

/// The deepest nesting `thread_scope_view::levels` can name.
/// Past it a thread still reports its true `depth`, with the innermost levels unnamed.
inline constexpr isize max_reported_scope_depth = 32;

/// Calls `f` once per registered thread with what that thread currently has open.
///
/// **Never waits and never suspends anything.**
/// The thread registry is a process-wide lock, and a thread that died inside registration still holds it — so a
/// busy registry returns false rather than hanging the report that was asked for.
/// False means `f` never ran.
///
/// **A thread appears only once it has recorded something**, which is how it joins the recorder's registry at all —
/// a log line, a scope, or the ambient delta an async worker writes on its own.
/// A thread that has done none of those is doing nothing a report could name anyway.
///
/// Allocation-free, so a crash handler may call it.
/// The views handed to `f` borrow the recorder's own memory and are valid only for the duration of the call.
[[nodiscard]] bool try_read_thread_scopes(cc::function_ref<void(rec::thread_scope_view const&)> f);

/// Writes every thread's open scopes to stderr, one thread per line-group.
///
/// The rendering half of the query, kept beside it so a crash handler and a hang report print the same thing.
/// Does nothing and says so when the registry is busy.
void report_thread_scopes(char const* reason) noexcept;
} // namespace cc::rec

/// One thread's open scopes, as far as its published events say.
struct cc::rec::thread_scope_view
{
    cc::thread_id id = cc::thread_id::invalid;

    /// The OS's own id, so this lines up with whatever else names threads.
    u64 native_tid = 0;

    u32 index = 0;

    /// What cc::rec::set_current_thread_record_name last set, or empty.
    cc::string_view name;

    /// False once the thread has exited; its state lingers until the recorder has drained it.
    bool is_alive = false;

    /// How many scopes are open, which may exceed what `levels` can name.
    u32 depth = 0;

    /// The open scopes, outermost first, and **null where the level is open but unnamed**.
    /// Shorter than `depth` when the thread is nested deeper than max_reported_scope_depth.
    cc::span<rec::desc const* const> levels;

    /// Whether every level in `levels` is named.
    /// False means the window did not reach far enough back, not that the thread is doing less.
    [[nodiscard]] bool is_complete() const
    {
        if (isize(depth) > levels.size())
            return false;
        for (auto const* const d : levels)
            if (d == nullptr)
                return false;
        return true;
    }
};
