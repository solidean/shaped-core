#pragma once

#include <clean-core/streams/impl/stream.hh>

#include <type_traits>

// =========================================================================================================
// Generic byte streams
// =========================================================================================================
//
// A stream is a NON-OWNING, MOVE-ONLY view over a byte buffer, driven by a single type-erased flush callback into an owning *adapter* (see span_stream.hh / file_stream.hh).
// The stream itself holds only the current window plus the callback and context:
//
//     byte* curr; byte* end; flush_fn flush; void* context;
//     (+ byte* first_write on write-capable streams; + byte* write_end on read_write streams)
//
// [curr, end) is the readable window (read streams) or the free-to-write window (write streams).
// The adapter behind `context` refills, drains and seeks on demand.
//
// A read_write stream needs BOTH boundaries at once, so it carries a second end: `end` is the read boundary (end of valid data) and `write_end` is the write capacity.
// On every other stream the write bound is just `end`, so `write_end` is zero-sized and, at the flush boundary, aliases `end`.
// Because these are separate, a buffered read_write adapter can still have free write space at EOF (`end == curr` but `write_end > curr`), so appending there just works.
// A span adapter sets both bounds to the same address, so its sink really is full once `curr == end`.
//
// PERFORMANCE.
// The stream is type-erased, but the hot path is NOT.
// Reads and writes act directly on the exposed [curr, end) buffer pointers.
// ready_bytes()/consume() and writable_bytes()/produce() are plain pointer moves, and read()/write()/read_pod()/write_pod() are immediate memcpy against that window.
// None of this goes through the function pointer or any other non-inlinable call.
// So even a long run of small consecutive operations stays fully inlined — a byte read is a load, not a virtual dispatch.
// The ONLY guaranteed non-inlinable call is `flush`, taken exactly when a window is exhausted or must be drained, and its cost is amortized across every byte moved between flushes.
//
// WHY A SINGLE FLUSH POINTER.
// Refill, drain, and all six seek_dir operations funnel through one function pointer, with dir + offset selecting the operation.
// A stream is therefore exactly {window + one pointer + context} no matter its access or seekability.
// So a single adapter, over one flush, can hand out any of the six stream types, with no per-operation vtable to translate.
//
// The flush contract itself — every parameter, the dry_* rules, the caller contract — is in stream_flush.hh, over libs/base/clean-core/docs/writing-a-stream.md.
// The one rule a stream USER needs from it: a stream is at its end iff `curr == end` AFTER a flush.
// A buffered adapter starts with an empty window, so a read consumer must flush once to get the first data; a span adapter hands out its whole window at construction.
//
// CONVERSIONS only ever NARROW — drop seekable, and read_write -> read or write; read and write are leaf capabilities that never cross.
// An adapter converts straight to its natural (most-capable) stream, or to any legal narrowing of it.
// A stream narrows to another stream too, but only FROM AN RVALUE: converting consumes the source and leaves it invalid, so a backend never ends up with two live views.
// That matters because the stream holds the curr/end window while flush is stateful in the adapter — a second overlay onto the same backend would desynchronize both.
// Temporarily downgrading a stream and getting the original back is a separate story, and not one this offers.
// Narrowing away write capability ASSERTS while writes are still pending: they can only drain through the source's own write bound, so flush before narrowing.

namespace cc
{
// Forward declarations so cc::impl::public_stream (below) can name the concrete types.
struct read_stream;
struct write_stream;
struct read_write_stream;
struct seekable_read_stream;
struct seekable_write_stream;
struct seekable_read_write_stream;
} // namespace cc

namespace cc::impl
{
// clang-format off
template <> struct public_stream<stream_access::read,        false> { using type = cc::read_stream; };
template <> struct public_stream<stream_access::write,       false> { using type = cc::write_stream; };
template <> struct public_stream<stream_access::read_write,  false> { using type = cc::read_write_stream; };
template <> struct public_stream<stream_access::read,        true>  { using type = cc::seekable_read_stream; };
template <> struct public_stream<stream_access::write,       true>  { using type = cc::seekable_write_stream; };
template <> struct public_stream<stream_access::read_write,  true>  { using type = cc::seekable_read_write_stream; };
// clang-format on
} // namespace cc::impl

namespace cc
{
// The six stream types.
// Each is a real, distinct type that PRIVATELY inherits the shared engine (cc::impl::stream<Access, Seekable>) and explicitly pulls in only the methods its capability supports.
// So the type's own definition IS its API, the way cc::vector lists its methods over allocating_container.
// Private inheritance keeps the engine hidden; adapters construct these directly (see span_stream.hh), and each type also takes an RVALUE of any wider stream, consuming it.
// The engine is befriended so try_as_seekable can build the seekable variant, and so a narrowing constructor can take the source's state.

/// Non-owning, move-only read view over a byte source.
/// Refills on demand via its adapter.
struct read_stream : private impl::stream<impl::stream_access::read, false>
{
    using engine = impl::stream<impl::stream_access::read, false>;
    template <impl::stream_access, bool>
    friend struct impl::stream;
    using engine::engine; // invalid stream; adapter bind (curr, end, flush, ctx)

    /// Narrow a wider stream, consuming it — the source is left invalid.
    template <class From>
        requires(!std::is_reference_v<From> && !std::is_same_v<From, read_stream>
                 && impl::stream_narrows_to<From, read_stream>)
    read_stream(From&& source)
    {
        this->engine::impl_narrow_from(source);
    }

    using engine::at_end;          // -> result<bool>
    using engine::consume;         // advance past n of ready_bytes()
    using engine::flush;           // -> result<i64>; refill the window
    using engine::is_valid;        // bound to an adapter?
    using engine::read;            // -> result<isize>; copy up to N, returns count
    using engine::read_all;        // -> result<vector<byte>>; whole remaining stream (one precise alloc when sized)
    using engine::read_exact;      // -> result<unit>; copy exactly N or error
    using engine::read_line;       // -> result<bool>; one line into a cc::string (optional max_size)
    using engine::read_pod;        // -> result<T>
    using engine::ready_bytes;     // -> span<byte const>; buffered bytes, viewed in place (no copy)
    using engine::try_as_seekable; // -> optional<seekable_read_stream>
};

/// Non-owning, move-only write view over a byte sink.
/// Drains on demand via its adapter.
struct write_stream : private impl::stream<impl::stream_access::write, false>
{
    using engine = impl::stream<impl::stream_access::write, false>;
    template <impl::stream_access, bool>
    friend struct impl::stream;
    using engine::engine;

    /// Narrow a wider stream, consuming it — the source is left invalid.
    template <class From>
        requires(!std::is_reference_v<From> && !std::is_same_v<From, write_stream>
                 && impl::stream_narrows_to<From, write_stream>)
    write_stream(From&& source)
    {
        this->engine::impl_narrow_from(source);
    }

    using engine::flush;           // -> result<i64>; drain pending writes
    using engine::is_valid;        //
    using engine::produce;         // advance past n written into writable_bytes()
    using engine::try_as_seekable; // -> optional<seekable_write_stream>
    using engine::writable_bytes;  // -> span<byte>; free write space, written in place (no copy)
    using engine::write;           // -> result<unit>; copy src in
    using engine::write_pod;       // -> result<unit>
};

/// Non-owning, move-only read+write view (e.g. over an in-memory span).
struct read_write_stream : private impl::stream<impl::stream_access::read_write, false>
{
    using engine = impl::stream<impl::stream_access::read_write, false>;
    template <impl::stream_access, bool>
    friend struct impl::stream;
    using engine::engine;

    /// Narrow a wider stream, consuming it — the source is left invalid.
    template <class From>
        requires(!std::is_reference_v<From> && !std::is_same_v<From, read_write_stream>
                 && impl::stream_narrows_to<From, read_write_stream>)
    read_write_stream(From&& source)
    {
        this->engine::impl_narrow_from(source);
    }

    using engine::at_end;
    using engine::consume;
    using engine::flush;
    using engine::is_valid;
    using engine::produce;
    using engine::read;
    using engine::read_all;
    using engine::read_exact;
    using engine::read_line;
    using engine::read_pod;
    using engine::ready_bytes;
    using engine::try_as_seekable; // -> optional<seekable_read_write_stream>
    using engine::writable_bytes;
    using engine::write;
    using engine::write_pod;
};

/// A read_stream that also supports fast seeking — O(1), or at worst O(log n). A source that can only
/// reposition by re-reading must present as a plain read_stream instead.
struct seekable_read_stream : private impl::stream<impl::stream_access::read, true>
{
    using engine = impl::stream<impl::stream_access::read, true>;
    template <impl::stream_access, bool>
    friend struct impl::stream;
    using engine::engine;

    /// Narrow a wider stream, consuming it — the source is left invalid.
    template <class From>
        requires(!std::is_reference_v<From> && !std::is_same_v<From, seekable_read_stream>
                 && impl::stream_narrows_to<From, seekable_read_stream>)
    seekable_read_stream(From&& source)
    {
        this->engine::impl_narrow_from(source);
    }

    using engine::at_end;
    using engine::consume;
    using engine::flush;
    using engine::is_valid;
    using engine::position; // -> result<i64>
    using engine::read;
    using engine::read_all;
    using engine::read_exact;
    using engine::read_line;
    using engine::read_pod;
    using engine::ready_bytes;
    using engine::remaining_bytes; // -> result<i64>
    using engine::seek_from_end;   // -> result<i64>
    using engine::seek_to;         // -> result<i64>; absolute from start
    using engine::size;            // -> result<i64>
    using engine::skip;            // -> result<i64>; relative
};

/// A write_stream that also supports fast (O(1)/O(log n)) seeking.
struct seekable_write_stream : private impl::stream<impl::stream_access::write, true>
{
    using engine = impl::stream<impl::stream_access::write, true>;
    template <impl::stream_access, bool>
    friend struct impl::stream;
    using engine::engine;

    /// Narrow a wider stream, consuming it — the source is left invalid.
    template <class From>
        requires(!std::is_reference_v<From> && !std::is_same_v<From, seekable_write_stream>
                 && impl::stream_narrows_to<From, seekable_write_stream>)
    seekable_write_stream(From&& source)
    {
        this->engine::impl_narrow_from(source);
    }

    using engine::flush;
    using engine::is_valid;
    using engine::position;
    using engine::produce;
    using engine::remaining_bytes;
    using engine::seek_from_end;
    using engine::seek_to;
    using engine::size;
    using engine::skip;
    using engine::writable_bytes;
    using engine::write;
    using engine::write_pod;
};

/// A read_write_stream that also supports fast (O(1)/O(log n)) seeking.
struct seekable_read_write_stream : private impl::stream<impl::stream_access::read_write, true>
{
    using engine = impl::stream<impl::stream_access::read_write, true>;
    template <impl::stream_access, bool>
    friend struct impl::stream;
    using engine::engine;

    using engine::at_end;
    using engine::consume;
    using engine::flush;
    using engine::is_valid;
    using engine::position;
    using engine::produce;
    using engine::read;
    using engine::read_all;
    using engine::read_exact;
    using engine::read_line;
    using engine::read_pod;
    using engine::ready_bytes;
    using engine::remaining_bytes;
    using engine::seek_from_end;
    using engine::seek_to;
    using engine::size;
    using engine::skip;
    using engine::writable_bytes;
    using engine::write;
    using engine::write_pod;
};
} // namespace cc
