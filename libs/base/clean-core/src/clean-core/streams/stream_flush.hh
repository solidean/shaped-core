#pragma once

#include <clean-core/common/utility.hh> // function_ptr
#include <clean-core/error/result.hh>
#include <clean-core/fwd.hh>

// =========================================================================================================
// The stream flush contract (public authoring API)
// =========================================================================================================
//
// An adapter — the owning type that holds the buffer and does the I/O — supplies ONE callback, the flush function below, and constructs a stream over its buffer:
//
//     cc::seekable_read_stream stream() { return cc::seekable_read_stream(buf, buf, &my_flush, this); }
//
// libs/base/clean-core/docs/writing-a-stream.md is the guide: how to write an adapter, a worked example, and the buffered / write / read_write cases.
// This block is the per-parameter reference for the signature below.
//
//   curr, end   - the current window, updated in place.
//                 For a readable stream `end` is the READ boundary (end of valid data); for a write-only stream it is the write capacity.
//   write_end   - the write capacity.
//                 On everything except a read_write stream this ALIASES `end` (same reference), so single-capability adapters can ignore it.
//                 A read_write adapter sets it apart from `end`, so that at EOF there is still free write space and append works.
//   ctx         - your adapter: the `this` you passed to the stream constructor.
//   seek_offset, dir - the requested op.
//                 (relative, 0) is a PLAIN FLUSH: refill the read window or write through the pending bytes, with no logical move.
//                 The dry_* dirs only COMPUTE the resulting global position, and must not touch curr/end/write_end or the buffer.
//                 remaining_size_hint is not a seek at all — see its own note below.
//   first_write - start of the pending writes to flush through: the bytes [first_write, curr) must be written to the sink.
//                 nullptr on reads, or when nothing is pending.
//                 Do NOT reset it — the stream does, after a successful non-dry flush.
//
// Return the global position of `curr` after the op, or -1 when the source has no meaningful position or is not seekable, or a cc::result error on I/O failure.
// A stream is at its end iff `curr == end` AFTER a flush.
//
// remaining_size_hint answers a different question and returns a COUNT rather than a position: how many bytes are still to come after `curr`.
// It exists so that a source which knows its own length can say so WITHOUT claiming to be seekable — a decompressing stream reading a frame header is the case it was added for.
// Answering it is optional; -1 means "no idea", which costs the caller one growing allocation and nothing else.
// try_as_seekable never asks it, so answering has no bearing on whether a stream can be upgraded.
//
// CALLER CONTRACT: the stream never calls flush with a dir outside its capability — a non-seekable stream issues no seeks, a read-only stream no write-through.
// So your flush may assert on an unsupported op rather than handle it, and an unsupported seek on a non-seekable source should return -1.

/// Where a seek offset is measured from, plus the one query that is not a seek.
/// The dry_* variants and remaining_size_hint only compute; they never move curr/end or disturb the buffer.
enum class cc::seek_dir : cc::u8
{
    begin,        // seek to `offset` bytes from the start
    relative,     // seek `offset` bytes from the current position; (relative, 0) is the plain flush
    end,          // seek to `offset` bytes from the end (offset <= 0 stays within the data)
    dry_begin,    // like begin, but only report the resulting position — no mutation, no I/O
    dry_relative, // like relative (dry_relative, 0 == current position; also the seekability probe)
    dry_end,      // like end (dry_end, 0 == total size)

    // NOT a seek: bytes still to come after `curr`, or -1 when unknown.
    // `offset` is ignored, and a non-seekable source may answer — which is the whole point of it being separate from the dry_* probes.
    remaining_size_hint,
};

namespace cc
{

/// The type-erased refill / drain / seek callback every stream adapter supplies.
/// The header comment above is the per-parameter reference, and libs/base/clean-core/docs/writing-a-stream.md is the guide.
using stream_flush_fn = cc::function_ptr<
    cc::result<i64>(byte*& curr, byte*& end, byte*& write_end, void* ctx, i64 seek_offset, seek_dir dir, byte* first_write)>;
} // namespace cc
