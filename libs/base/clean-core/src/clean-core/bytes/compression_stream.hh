#pragma once

#include <clean-core/bytes/compression.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/fwd.hh>
#include <clean-core/streams/stream.hh>

// Compression as a stream filter (bytes/).
//
// These are the first adapters in clean-core that wrap another stream rather than a buffer or a file handle, so a
// parser reads decompressed bytes straight off the window with nothing copied in between:
//
//     auto file = cc::file_read_stream_adapter::open(path).value();
//     auto gz = cc::decompressing_read_stream_adapter::create(file.stream(), {.algorithm = cc::compression_algorithm::deflate}).value();
//     cc::read_stream in = gz.stream();
//     auto const doc = babel::json::read(in);
//
// The algorithm is not optional on the reading side: nothing has been read yet to sniff when the context is created.
//
// Every framing but `raw` streams.
// A raw lz4 blob is one block with no continuation, a raw zstd blob has no header to resume from, and raw deflate is a
// bare stream with nothing to resume into, so a stream over any of them would have nothing to work with; `raw` is
// rejected at create().
// Deflate's `zlib` wrapper — a header and a trailing Adler-32 — streams exactly as gzip and the lz4 and zstd frames do.
//
// WHICH decompression_config FIELDS THESE HONOUR.
// `algorithm` (required), `framing` and `max_output_size` all bind; `dictionary` does not, and a stream that asks for
// one fails on the first read rather than at create().
//
// LIFETIME, and it is two levels deep here.
// A stream borrows into its adapter's inline buffer, so the adapter must outlive any stream taken from it and must
// not be moved once one is live — the usual rule, from ../../../docs/writing-a-stream.md.
// On top of that, these adapters hold an INNER stream by value, which borrows into ITS adapter.
// So the inner adapter must outlive this one, and neither hop is something the compiler can check for you.

/// Decompresses an inner read_stream on demand.
///
/// Not seekable, and it cannot be made so: reaching an earlier offset would mean decoding the frame again from the start.
/// It does answer seek_dir::remaining_size_hint when the frame declares its size in the HEADER — zstd and lz4 — which is what lets read_all() size its buffer in one allocation.
/// gzip keeps its size in the trailer, which a stream holding one window has not reached, so a deflate stream reports no hint at all.
struct cc::decompressing_read_stream_adapter
{
    /// Fails on a config this cannot stream, and on a create() the backend refuses.
    /// The frame itself is not touched here, so a corrupt one surfaces on the first read rather than now.
    [[nodiscard]] static cc::result<decompressing_read_stream_adapter> create(cc::read_stream&& inner,
                                                                              decompression_config cfg = {});

    ~decompressing_read_stream_adapter();

    // Movable only until a stream is taken: the flush callback captures `this` and the window points into _buffer.
    // Moving afterwards leaves that stream pointing at the old address, and create() returning one is the intended move.
    decompressing_read_stream_adapter(decompressing_read_stream_adapter&& rhs) noexcept;
    decompressing_read_stream_adapter& operator=(decompressing_read_stream_adapter&& rhs) noexcept;

    [[nodiscard]] cc::read_stream stream();

private:
    decompressing_read_stream_adapter() = default;

    static cc::result<i64>
    impl_flush(byte*& curr, byte*& end, byte*& write_end, void* ctx, i64 offset, seek_dir dir, byte* first_write);
    [[nodiscard]] cc::result<i64> impl_refill(byte*& curr, byte*& end);

    static constexpr isize k_buffer_size = 16384;

    cc::read_stream _inner;
    decompression_config _config;
    compression_algorithm _algorithm = {};
    void* _state = nullptr;

    /// Decompressed bytes still to come, or -1 when the frame declares no size in its header.
    /// Read once off the first window, clamped to max_output_size, then decremented as bytes are produced.
    i64 _remaining_hint = -1;

    /// Total decompressed bytes handed out so far, which is what max_output_size is measured against.
    i64 _total_produced = 0;

    bool _hint_probed = false;
    bool _frame_finished = false;

    byte _buffer[k_buffer_size] = {};
};

/// Compresses into an inner write_stream.
///
/// finish() MUST be called before this is destroyed, and it is not optional bookkeeping.
/// A frame ends with a terminator and a checksum that only finish() writes, so a frame that is never sealed cannot be decompressed at all.
/// The stream engine has no end-of-stream concept to hang it on and a destructor cannot report the I/O error, which is why it is an explicit call the destructor then asserts on.
struct cc::compressing_write_stream_adapter
{
    [[nodiscard]] static cc::result<compressing_write_stream_adapter> create(cc::write_stream&& inner,
                                                                             compression_config cfg = {});

    ~compressing_write_stream_adapter();

    // Same rule as above: movable until a stream is taken, pinned after.
    compressing_write_stream_adapter(compressing_write_stream_adapter&& rhs) noexcept;
    compressing_write_stream_adapter& operator=(compressing_write_stream_adapter&& rhs) noexcept;

    [[nodiscard]] cc::write_stream stream();

    /// Drain whatever is buffered, seal the frame, and return the total bytes handed to the inner stream.
    /// Writing to the stream afterwards asserts.
    /// The inner stream is NOT flushed — that stays its owner's job.
    [[nodiscard]] cc::result<i64> finish();

private:
    compressing_write_stream_adapter() = default;

    static cc::result<i64>
    impl_flush(byte*& curr, byte*& end, byte*& write_end, void* ctx, i64 offset, seek_dir dir, byte* first_write);
    [[nodiscard]] cc::result<cc::unit> impl_emit(cc::span<byte const> in, bool finish);

    static constexpr isize k_buffer_size = 16384;

    cc::write_stream _inner;
    compression_config _config;
    void* _state = nullptr;

    /// Staging for one compressed chunk, sized from the backend's bound for a full buffer.
    cc::vector<byte> _staging;

    i64 _written = 0;
    bool _finished = false;

    byte _buffer[k_buffer_size] = {};
};
