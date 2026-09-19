#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
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

/// Where a dump's bytes go.
///
/// **A seam rather than a path**, because the destination is policy the application owns.
/// A file is right under node and on every native platform, and there is no filesystem in a browser at all: what a
/// page does with a dump — a download, IndexedDB, a POST to a collector — is not clean-core's to pick.
///
/// A sink must survive whatever it is installed for.
/// The crash path calls it from a fault handler, so an implementation may not allocate and may not take a lock any
/// thread could be holding; the hang path is under no such restriction and says so through `dump_mode`.
struct cc::rec::dump_sink
{
    virtual ~dump_sink() = default;

    /// Writes all of `bytes`, or returns false — after which the dump stops rather than writing a torn file.
    [[nodiscard]] virtual bool write(cc::span<byte const> bytes) = 0;

    /// Called once when the dump ends, whether or not it succeeded.
    virtual void finish(bool ok) { (void)ok; }
};

/// What a dump is allowed to do, which differs by what went wrong.
enum class cc::rec::dump_mode
{
    /// A fault: no allocation, no waiting on any lock, and the consumer left running.
    ///
    /// The cost is a race the crash path cannot close — the actor may recycle a chunk while the dump reads the queue
    /// behind it — and it is accepted because the alternative is a handler that hangs.
    constrained,

    /// A hang: the consumer is stopped first, so that race is closed.
    ///
    /// Legal because a hang is not a fault, which is exactly what a fault handler cannot assume.
    /// The wait for the consumer is bounded: one that cannot catch up — a thread recording in a tight loop keeps it
    /// busy — gets a constrained dump instead, and the result says which one was written.
    quiescent,
};

/// What a crash dump is allowed to cost, decided at install time because a crash handler cannot decide anything.
struct cc::rec::crash_dump_options
{
    /// Where the dump goes when no `sink` is given.
    /// Copied at install time: formatting a path inside a crash handler would allocate.
    cc::string_view path;

    /// A destination of the caller's own, which takes precedence over `path`.
    ///
    /// **Must outlive the process's crash handling** — a static, or something deliberately leaked.
    /// This is the browser's route, and anything else that is not a file.
    rec::dump_sink* sink = nullptr;

    /// The scratch the table builder gets, reserved now.
    /// Four megabytes covers a few thousand distinct recording sites, which is far more than a program has.
    isize arena_bytes = 4 << 20;

    /// Stop after this many bytes of events, so a dump of a long-running process stays a file somebody can open.
    /// Hitting it marks the result truncated rather than failing it.
    isize max_event_bytes = 256 << 20;

    /// Seal the calling thread's chunk first, so the events that led up to the crash are in the dump rather than
    /// waiting for a chunk that will never fill.
    bool seal_calling_thread = true;

    /// How long a `quiescent` dump waits for the consumer before settling for a constrained one.
    /// A drain pass is normally well under a millisecond, so this is only reached by a consumer that cannot catch up.
    double consumer_pause_timeout_secs = 1.0;
};

namespace cc::rec
{
/// Writes a dump to `sink` right now, on the calling thread, and returns the mode it was actually written under.
///
/// The one writer both paths go through, told which constraints it is under rather than guessing.
/// Requires an installed dump, since the arena and the module table it needs are reserved at install time.
/// A `quiescent` request comes back `constrained` when the consumer did not yield in time.
///
/// Empty when nothing is installed, when the recorder is down, when the registry was busy, or when the sink refused
/// a write.
[[nodiscard]] cc::optional<rec::dump_mode> write_dump(rec::dump_sink& sink, rec::dump_mode mode);

/// Installs a crash-context hook that dumps every thread's committed events.
///
/// Reserves its arena now, so the handler itself allocates nothing.
/// Idempotent in the sense that a second call replaces the options; cc::install_crash_handler must have been called
/// too, since this rides its hook list.
void install_crash_dump(rec::crash_dump_options const& options);

/// Writes a dump right now, on the calling thread, through the installed destination.
///
/// `constrained` is exactly the path the crash handler takes, which is what tests it: the fault-path writer is not
/// something to find out about during a fault.
/// `quiescent` is the hang path, which stops the consumer first and so may wait, for up to a second.
///
/// Returns the mode the dump was written under, as write_dump does, or empty when no dump is installed or the
/// destination refused a write.
[[nodiscard]] cc::optional<rec::dump_mode> write_dump_now(rec::dump_mode mode);

/// The path the installed dump writes to, or empty — including when a `sink` was given instead.
[[nodiscard]] cc::string_view crash_dump_path();

/// Whether a dump is installed at all, and so whether write_dump can do anything.
[[nodiscard]] bool is_crash_dump_installed();
} // namespace cc::rec
