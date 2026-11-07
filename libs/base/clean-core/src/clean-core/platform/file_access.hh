#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/atomic.hh>

// =========================================================================================================
// cc::file_access — opt-in recording of every path the process reads, writes or lists
// =========================================================================================================
//
// Off by default, and then free: every note is one relaxed atomic load and a branch.
// Turned on (start_recording), clean-core's file functions note each path they open, with the bytes moved through it,
// so a caller can learn what a run depended on and what it produced: a test runner caching results, an IO report.
//
// What notes itself: the file streams (file_read_stream_adapter, file_write_stream_adapter,
// file_read_write_stream_adapter), cc::rec's recording files, cc::remove_file.
// A failed open is noted too, since a file found missing is as much an input as one found present.
// The crash dump writes through native_file directly and notes nothing: it must not take a lock while the process dies.
//
// **Code that touches files past clean-core is not seen** — std::filesystem, fopen, a third-party library's own IO.
// Such code declares what it touches with note_read / note_write / note_list; undeclared, the access is not recorded.
// clean-core has no directory iteration of its own, so every directory listing is such a declaration.
//
// Attribution: each note carries a context, a u64 a hook supplies (set_context_hook).
// A test runner installs one naming the running test, read from wherever it tracks that, typically cc's ambient chain,
// which follows work across threads and async tasks; clean-core never learns what a context means.
// Without a hook, or outside any context, the context is 0.

/// One path as recorded under one context.
struct cc::file_access::record
{
    /// What the context hook returned when the path was noted; 0 for none.
    u64 context = 0;
    /// As the code named it: absolute or relative to the working directory, separators as given.
    cc::string path;
    bool read = false;
    bool written = false;
    bool listed = false;
    /// Bytes moved through clean-core's file streams; a declared access moves none.
    i64 bytes_read = 0;
    i64 bytes_written = 0;
};

namespace cc::file_access
{
/// The hook naming the context of a note, called on the noting thread; return 0 for none.
using context_hook = u64 (*)();
} // namespace cc::file_access

namespace cc::file_access::impl
{
enum class access : u8
{
    read,
    write,
    list,
};

/// The byte counters of one recorded (context, path); stable for the life of the process once handed out.
struct counters
{
    cc::atomic<i64> bytes_read = {0};
    cc::atomic<i64> bytes_written = {0};
};

extern cc::atomic<bool> g_recording;

[[nodiscard]] counters* note_slow(access kind, cc::string_view path);

/// Notes `path`, returning its counters for the bytes a stream then moves; null while not recording.
inline counters* note(access kind, cc::string_view path)
{
    if (!g_recording.load(cc::memory_order_relaxed)) [[likely]]
        return nullptr;
    return note_slow(kind, path);
}

inline void add_read(counters* c, i64 bytes)
{
    if (c != nullptr)
        c->bytes_read.fetch_add(bytes, cc::memory_order_relaxed);
}

inline void add_written(counters* c, i64 bytes)
{
    if (c != nullptr)
        c->bytes_written.fetch_add(bytes, cc::memory_order_relaxed);
}
} // namespace cc::file_access::impl

namespace cc::file_access
{
/// Whether accesses are being recorded now.
[[nodiscard]] inline bool is_recording()
{
    return impl::g_recording.load(cc::memory_order_relaxed);
}

/// Starts recording, process-wide; what is already recorded stays.
void start_recording();

/// Stops recording; what is recorded stays readable through snapshot().
void stop_recording();

/// Installs the hook naming each note's context; null removes it.
/// Install it before start_recording: a note racing the install may take either hook.
void set_context_hook(context_hook hook);

/// The context a note made here and now would carry.
[[nodiscard]] u64 current_context();

/// Declares that the calling code read `path` past clean-core (std::filesystem, fopen, a library's own IO).
inline void note_read(cc::string_view path)
{
    (void)impl::note(impl::access::read, path);
}

/// Declares that the calling code wrote, created or deleted `path` past clean-core.
inline void note_write(cc::string_view path)
{
    (void)impl::note(impl::access::write, path);
}

/// Declares that the calling code listed the entries of `directory`.
inline void note_list(cc::string_view directory)
{
    (void)impl::note(impl::access::list, directory);
}

/// Everything recorded so far, one record per (context, path), in no particular order.
[[nodiscard]] cc::vector<record> snapshot();
} // namespace cc::file_access
