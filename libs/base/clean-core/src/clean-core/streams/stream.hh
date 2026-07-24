#pragma once

#include <clean-core/streams/impl/stream.hh>

// =========================================================================================================
// Generic byte streams
// =========================================================================================================
//
// A stream is a NON-OWNING, MOVE-ONLY view over a byte buffer, driven by a single type-erased flush callback
// into an owning *adapter* (see span_stream.hh / file_stream.hh). The stream itself holds only the current
// window plus the callback + context:
//
//     byte* curr; byte* end; flush_fn flush; void* context;
//     (+ byte* first_write on write-capable streams; + byte* write_end on read_write streams)
//
// [curr, end) is the readable window (read streams) or the free-to-write window (write streams). The adapter
// behind `context` refills / drains / seeks on demand.
//
// A read_write stream needs BOTH boundaries at once, so it carries a second end: `end` is the read boundary
// (end of valid data) and `write_end` is the write capacity. On every other stream the write bound is just
// `end`, so `write_end` is zero-sized and, at the flush boundary, aliases `end`. Because these are separate,
// a read_write stream at EOF still has free write space (`end == curr` but `write_end > curr`), so appending
// there just works — no ambiguity between "refill for read" and "make room to write".
//
// PERFORMANCE. The stream is type-erased, but the hot path is NOT. Reads and writes act directly on the
// exposed [curr, end) buffer pointers: ready_bytes()/consume() and writable_bytes()/produce() are plain pointer
// moves, and read()/write()/read_pod()/write_pod() are immediate memcpy against that window. None of this
// goes through the function pointer or any other non-inlinable call, so even a long run of small consecutive
// operations stays fully inlined — a byte read is a load, not a virtual dispatch. The ONLY guaranteed
// (non-inlinable) call is `flush`, taken exactly when a window is exhausted or must be drained; its cost is
// amortized across every byte moved between flushes.
//
// WHY A SINGLE FLUSH POINTER. Refill, drain, and all six seek_dir operations funnel through one function
// pointer (dir + offset select the operation). A stream is therefore exactly {window + one pointer + context}
// no matter its access or seekability — so a single adapter, over one flush, can hand out any of the six
// stream types, with no per-operation vtable to translate.
//
// Flush contract (dir + offset select the operation; see cc::seek_dir in stream_flush.hh):
//   * (relative, 0) is a PLAIN FLUSH: refill the read window / write through pending bytes, no logical move.
//   * A stream is at its end iff `curr == end` AFTER a flush — for reads that means "no more data", for a
//     bounded write sink (e.g. a fixed span) it means "no more space". Unbounded sinks (files) keep
//     curr < end after a successful write-flush. Initially curr == end, so a read consumer must flush once
//     to obtain the first data (span adapters are unbuffered and hand out a full window up front).
//   * dry_* variants compute the resulting global position WITHOUT touching curr/end or the buffer; used by
//     position()/size()/remaining_bytes() and to probe seekability cheaply.
//   * flush returns the global position of `curr`, or -1 when the source has no meaningful position / is not
//     seekable, or a cc::result error on I/O failure.
//   * first_write is passed by value and reset by the stream wrapper (not by flush) after a successful,
//     non-dry flush; on error it is left intact so the write can be retried.
//   * CALLER CONTRACT: a stream must never invoke flush with parameters outside its own capability. A
//     non-seekable stream must not issue any seek — even if the underlying adapter could seek — and a read
//     stream must not request a write-through. The public API enforces this by construction (seek_* exist
//     only on seekable streams, write-through only happens on write streams), so the flush callback trusts
//     its inputs and asserts on a violation rather than defensively checking every call.
//
// CONVERSIONS happen at the ADAPTER, not between streams. An adapter converts straight to its natural
// (most-capable) stream or to any legal NARROWING of it — drop seekable, and read_write -> read or write;
// read and write are leaf capabilities that never cross. A stream itself, once made, is its type: there is no
// stream-to-stream conversion (only move, which invalidates the source — streams are move-only).

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
// The six stream types. Each is a real, distinct type that PRIVATELY inherits the shared engine
// (cc::impl::stream<Access, Seekable>) and explicitly pulls in only the methods its capability supports — so
// the type's own definition IS its API, the way cc::vector lists its methods over allocating_container.
// Private inheritance keeps the engine hidden; adapters construct these directly and there is no
// stream-to-stream conversion (adapters convert straight to any narrower type — see span_stream.hh). The
// engine is befriended so try_as_seekable can build the seekable variant.

/// Non-owning, move-only read view over a byte source. Refills on demand via its adapter.
struct read_stream : private impl::stream<impl::stream_access::read, false>
{
    using engine = impl::stream<impl::stream_access::read, false>;
    template <impl::stream_access, bool>
    friend struct impl::stream;
    using engine::engine; // invalid stream; adapter bind (curr, end, flush, ctx)

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

/// Non-owning, move-only write view over a byte sink. Drains on demand via its adapter.
struct write_stream : private impl::stream<impl::stream_access::write, false>
{
    using engine = impl::stream<impl::stream_access::write, false>;
    template <impl::stream_access, bool>
    friend struct impl::stream;
    using engine::engine;

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
