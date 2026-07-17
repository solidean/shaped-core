#pragma once

#include <clean-core/error/result.hh>
#include <clean-core/fwd.hh>
#include <clean-core/streams/impl/native_file.hh>
#include <clean-core/streams/stream.hh>
#include <clean-core/string/string_view.hh>

#include <type_traits>

// =========================================================================================================
// File stream adapters
// =========================================================================================================
//
// Buffered, seekable adapters backed by an OS file (via cc::impl::native_file). Each owns a 4 KiB inline
// buffer; the flush callback reads/writes through it and maps seeks onto the OS file pointer.
//
// LIFETIME: the stream's window points into the adapter's inline buffer, so the adapter must outlive any
// stream taken from it. An adapter may be moved BEFORE a stream is taken; once `.stream()` (or an implicit
// conversion) has produced a live stream, the adapter is effectively pinned and must not be moved.
//
// Construct via the static open()/create()/append() factories; the adapter then converts implicitly to the
// matching stream, or hand out one explicitly with `.stream()`.
//
// GROWTH. Writing grows the file whenever the window maps to (or past) the end. For the write adapter the
// differentiation is at the factory: create (truncate), open (overwrite in place), append (start at EOF).
// The read_write adapter grows too — a write past the current end extends the file, including a fresh
// seek-to-end + write (it keeps the read boundary and the write capacity as separate ends).

namespace cc
{
/// Buffered read adapter over a file. Hands out a seekable_read_stream.
class file_read_stream_adapter
{
public:
    static constexpr cc::isize k_buffer_size = 4096;

    /// Open an existing file for reading.
    [[nodiscard]] static cc::result<file_read_stream_adapter> open(cc::string_view path);

    file_read_stream_adapter(file_read_stream_adapter&&) noexcept = default;
    file_read_stream_adapter& operator=(file_read_stream_adapter&&) noexcept = default;
    file_read_stream_adapter(file_read_stream_adapter const&) = delete;
    file_read_stream_adapter& operator=(file_read_stream_adapter const&) = delete;
    ~file_read_stream_adapter() = default;

    /// Hand out the read stream. Starts empty (curr == end), so the first read triggers a flush that fills
    /// the buffer.
    [[nodiscard]] cc::seekable_read_stream stream()
    {
        return cc::seekable_read_stream(_buffer, _buffer, &impl_flush, this);
    }

    template <class Stream>
        requires cc::impl::stream_narrows_to<cc::seekable_read_stream, Stream>
    operator Stream()
    {
        return Stream(_buffer, _buffer, &impl_flush, this);
    }

private:
    file_read_stream_adapter() = default;

    static cc::result<cc::i64> impl_flush(cc::byte*& curr,
                                          cc::byte*& end,
                                          cc::byte*& write_end,
                                          void* ctx,
                                          cc::i64 offset,
                                          cc::seek_dir dir,
                                          cc::byte* first_write);
    cc::result<cc::i64> impl_seek_and_fill(cc::byte*& curr, cc::byte*& end, cc::i64 target);

    cc::impl::native_file _file;
    cc::i64 _buffer_offset = 0; // absolute file offset of _buffer[0]
    cc::byte _buffer[k_buffer_size];
};

/// Buffered write adapter over a file. Hands out a seekable_write_stream. Unbounded: a write-flush always
/// frees the whole buffer (curr < end) unless the disk errors.
class file_write_stream_adapter
{
public:
    static constexpr cc::isize k_buffer_size = 4096;

    /// Create or truncate a file for writing (the stream starts at offset 0).
    [[nodiscard]] static cc::result<file_write_stream_adapter> create(cc::string_view path);
    /// Open an existing file for writing WITHOUT truncating; the stream starts at offset 0 (overwrite in
    /// place, seek to move). Creates the file if missing.
    [[nodiscard]] static cc::result<file_write_stream_adapter> open(cc::string_view path);
    /// Open for appending: the stream starts positioned at the current end of the file, so writing grows it.
    /// Creates the file if missing.
    [[nodiscard]] static cc::result<file_write_stream_adapter> append(cc::string_view path);

    file_write_stream_adapter(file_write_stream_adapter&&) noexcept = default;
    file_write_stream_adapter& operator=(file_write_stream_adapter&&) noexcept = default;
    file_write_stream_adapter(file_write_stream_adapter const&) = delete;
    file_write_stream_adapter& operator=(file_write_stream_adapter const&) = delete;
    ~file_write_stream_adapter() = default;

    /// Hand out the write stream. Starts with the whole buffer free (curr = buffer, end = buffer + size).
    [[nodiscard]] cc::seekable_write_stream stream()
    {
        return cc::seekable_write_stream(_buffer, _buffer + k_buffer_size, &impl_flush, this);
    }

    template <class Stream>
        requires cc::impl::stream_narrows_to<cc::seekable_write_stream, Stream>
    operator Stream()
    {
        return Stream(_buffer, _buffer + k_buffer_size, &impl_flush, this);
    }

private:
    file_write_stream_adapter() = default;

    static cc::result<cc::i64> impl_flush(cc::byte*& curr,
                                          cc::byte*& end,
                                          cc::byte*& write_end,
                                          void* ctx,
                                          cc::i64 offset,
                                          cc::seek_dir dir,
                                          cc::byte* first_write);
    cc::result<cc::i64> impl_write_through(cc::byte*& curr, cc::byte*& end, cc::byte* first_write, cc::i64 pos);
    cc::result<cc::i64> impl_reposition(cc::byte*& curr, cc::byte*& end, cc::i64 target);

    cc::impl::native_file _file;
    cc::i64 _buffer_offset = 0; // absolute file offset of _buffer[0]
    cc::byte _buffer[k_buffer_size];
};

/// Buffered read+write adapter over a file. Hands out a seekable_read_write_stream: seekable reads, in-place
/// overwrite, and growth — writing past the current end extends the file, including a fresh seek-to-end + write
/// with nothing buffered. This works because the stream tracks the read boundary and the write capacity as two
/// separate ends (so at EOF there is still free write space), rather than overloading a single window.
class file_read_write_stream_adapter
{
public:
    static constexpr cc::isize k_buffer_size = 4096;

    /// Open an existing file for reading and writing.
    [[nodiscard]] static cc::result<file_read_write_stream_adapter> open(cc::string_view path);

    file_read_write_stream_adapter(file_read_write_stream_adapter&&) noexcept = default;
    file_read_write_stream_adapter& operator=(file_read_write_stream_adapter&&) noexcept = default;
    file_read_write_stream_adapter(file_read_write_stream_adapter const&) = delete;
    file_read_write_stream_adapter& operator=(file_read_write_stream_adapter const&) = delete;
    ~file_read_write_stream_adapter() = default;

    /// Hand out the read+write stream. Starts empty (curr == end), so the first op triggers a flush.
    [[nodiscard]] cc::seekable_read_write_stream stream()
    {
        return cc::seekable_read_write_stream(_buffer, _buffer, &impl_flush, this);
    }

    template <class Stream>
        requires cc::impl::stream_narrows_to<cc::seekable_read_write_stream, Stream>
    operator Stream()
    {
        return Stream(_buffer, _buffer, &impl_flush, this);
    }

private:
    file_read_write_stream_adapter() = default;

    static cc::result<cc::i64> impl_flush(cc::byte*& curr,
                                          cc::byte*& end,
                                          cc::byte*& write_end,
                                          void* ctx,
                                          cc::i64 offset,
                                          cc::seek_dir dir,
                                          cc::byte* first_write);
    cc::result<cc::i64> impl_drain(cc::byte* curr, cc::byte* first_write);
    cc::result<cc::i64> impl_fill(cc::byte*& curr, cc::byte*& end, cc::byte*& write_end, cc::isize leftover);
    cc::result<cc::i64> impl_seek_and_fill(cc::byte*& curr, cc::byte*& end, cc::byte*& write_end, cc::i64 target);

    cc::impl::native_file _file;
    cc::i64 _buffer_offset = 0; // absolute file offset of _buffer[0]
    cc::byte _buffer[k_buffer_size];
};
} // namespace cc
