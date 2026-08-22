#pragma once

#include <clean-core/record/fwd.hh>
#include <clean-core/string/string_view.hh>

// Writing every thread's events out from inside a crash handler.
//
// The constraint that shapes all of this: **the dump must not allocate.**
// A crash inside the allocator is exactly the case where a dump is most wanted and an allocating writer would
// deadlock, so the arena it builds its tables in is reserved at install time and the path is copied then too.
//
// **No thread is suspended, and none needs to be.**
// A chunk's committed watermark is release-stored after its bytes, so reading up to it can never catch a torn event.
// The worst a live thread costs the dump is its newest event, which is a far better trade than the deadlock risk of
// suspending threads that may hold the loader lock.
//
// **Nor does the dump ever wait on a lock**, for the same reason it never allocates: a thread that crashed holding the
// recorder's thread registry would hang a dump that waited for it.
// The walk is a try-lock, and failing it means no dump rather than a hang.
//
// What the dump does NOT do yet is stop the consumer, so the actor may be recycling a chunk while the dump reads the
// queue behind it — a race that turns a rare crash into a rarer second one.
// Stopping the actor from a handler is its own hazard (it means joining a thread that may itself be stuck), so this is
// deliberately left until that is designed rather than papered over.
//
// The result is an ordinary recording file — the same format cc::rec::serialize writes — so it loads through
// cc::rec::load_recording like any other, and needs no separate reader.

/// What a crash dump is allowed to cost, decided at install time because a crash handler cannot decide anything.
struct cc::rec::crash_dump_options
{
    /// Where the dump goes.
    /// Copied at install time: formatting a path inside a crash handler would allocate.
    cc::string_view path;

    /// The scratch the table builder gets, reserved now.
    /// Four megabytes covers a few thousand distinct recording sites, which is far more than a program has.
    isize arena_bytes = 4 << 20;

    /// Stop after this many bytes of events, so a dump of a long-running process stays a file somebody can open.
    /// Hitting it marks the result truncated rather than failing it.
    isize max_event_bytes = 256 << 20;

    /// Seal the calling thread's chunk first, so the events that led up to the crash are in the dump rather than
    /// waiting for a chunk that will never fill.
    bool seal_calling_thread = true;
};

namespace cc::rec
{
/// Installs a crash-context hook that dumps every thread's committed events.
///
/// Reserves its arena now, so the handler itself allocates nothing.
/// Idempotent in the sense that a second call replaces the options; cc::install_crash_handler must have been called
/// too, since this rides its hook list.
void install_crash_dump(rec::crash_dump_options const& options);

/// Writes the dump right now, on the calling thread, through exactly the path the crash handler takes.
///
/// This is what tests the crash path: the constrained writer is not something to find out about during a crash.
/// Returns false when no dump is installed, or when the file could not be written.
[[nodiscard]] bool write_crash_dump_now();

/// The path the installed dump writes to, or empty.
[[nodiscard]] cc::string_view crash_dump_path();
} // namespace cc::rec
