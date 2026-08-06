#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/streams/stream.hh>

#include <cstring> // std::memmove, std::memcpy

// Shared test helpers + deliberately non-seekable mock adapters, used to exercise the parts of the stream
// contract that the (always-seekable) span and file adapters can't reach: flush returning -1, try_as_seekable
// failing, and the first_write write-through / reset behaviour on a recording sink.

namespace cc_stream_test
{
// Vocabulary types (i32/u32/u64/isize/byte/...) available bare inside cc_stream_test, not leaked globally.
using namespace cc::primitive_defines;

inline byte b(int v)
{
    return byte(v);
}

inline bool bytes_equal(cc::span<byte const> a, cc::span<byte const> b)
{
    if (a.size() != b.size())
        return false;
    return a.empty() || std::memcmp(a.data(), b.data(), size_t(a.size())) == 0;
}

/// A non-seekable in-memory READ source, modelling a pipe.
/// It serves bytes from a fixed source in fixed-size chunks, and returns -1 for every seek and dry-seek, so try_as_seekable must fail on it.
/// A plain flush (relative, 0) still works, and also returns -1, since there is no meaningful position.
class mock_pipe_read_stream_adapter
{
public:
    mock_pipe_read_stream_adapter(cc::span<byte const> data, isize chunk) : _data(data), _chunk(chunk) {}

    mock_pipe_read_stream_adapter(mock_pipe_read_stream_adapter&&) = delete; // pinned: the stream borrows _buffer
    mock_pipe_read_stream_adapter& operator=(mock_pipe_read_stream_adapter&&) = delete;

    [[nodiscard]] cc::read_stream stream() { return cc::read_stream(_buffer, _buffer, &impl_flush, this); }

private:
    static cc::result<i64> impl_flush(byte*& curr,
                                      byte*& end,
                                      byte*& /*write_end*/, // aliases end for a read-only stream
                                      void* ctx,
                                      i64 offset,
                                      cc::seek_dir dir,
                                      byte* /*first_write*/)
    {
        auto& self = *static_cast<mock_pipe_read_stream_adapter*>(ctx);
        if (!(dir == cc::seek_dir::relative && offset == 0))
            return i64(-1); // not seekable

        byte* const base = self._buffer;
        isize const leftover = isize(end - curr);
        std::memmove(base, curr, size_t(leftover));

        isize const room = isize(sizeof(self._buffer)) - leftover;
        isize const want = cc::min(self._chunk, room);
        isize const avail = self._data.size() - self._pos;
        isize const n = cc::min(want, avail);
        if (n > 0)
            std::memcpy(base + leftover, self._data.data() + self._pos, size_t(n));
        self._pos += n;

        curr = base;
        end = base + leftover + n;
        return i64(-1); // a pipe has no meaningful position
    }

    cc::span<byte const> _data;
    isize _chunk;
    isize _pos = 0;
    byte _buffer[64];
};

/// A non-seekable in-memory WRITE sink that records every write-through range it is handed, so tests can
/// verify first_write is set on write and reset after each flush.
class recording_write_stream_adapter
{
public:
    recording_write_stream_adapter() = default;
    recording_write_stream_adapter(recording_write_stream_adapter&&) = delete; // pinned: the stream borrows _buffer
    recording_write_stream_adapter& operator=(recording_write_stream_adapter&&) = delete;

    [[nodiscard]] cc::write_stream stream() { return cc::write_stream(_buffer, _buffer + k_cap, &impl_flush, this); }

    [[nodiscard]] cc::span<byte const> written() const { return _sink; }
    [[nodiscard]] int flushes_with_pending() const { return _flushes_with_pending; }

private:
    static constexpr isize k_cap = 64;

    static cc::result<i64> impl_flush(byte*& curr,
                                      byte*& end,
                                      byte*& /*write_end*/, // aliases end for a write-only stream
                                      void* ctx,
                                      i64 offset,
                                      cc::seek_dir dir,
                                      byte* first_write)
    {
        auto& self = *static_cast<recording_write_stream_adapter*>(ctx);
        if (!(dir == cc::seek_dir::relative && offset == 0))
            return i64(-1); // not seekable

        if (first_write != nullptr && curr > first_write)
        {
            for (byte* p = first_write; p != curr; ++p)
                self._sink.push_back(*p);
            ++self._flushes_with_pending;
        }
        curr = self._buffer;
        end = self._buffer + k_cap;
        return i64(-1);
    }

    cc::vector<byte> _sink;
    int _flushes_with_pending = 0;
    byte _buffer[k_cap];
};

/// A read_write adapter whose read boundary and write capacity are deliberately DIFFERENT: a flush hands out `readable` bytes to read, but the whole buffer to write into.
/// On a span adapter the two always coincide, so this is the only shape that can tell `end` (read) and `write_end` (write) apart.
/// Which is exactly what a read_write -> write narrowing has to get right.
class mock_split_bounds_read_write_adapter
{
public:
    static constexpr isize k_cap = 16;

    explicit mock_split_bounds_read_write_adapter(isize readable) : _readable(readable) {}

    mock_split_bounds_read_write_adapter(mock_split_bounds_read_write_adapter&&) = delete; // pinned: borrowed _buffer
    mock_split_bounds_read_write_adapter& operator=(mock_split_bounds_read_write_adapter&&) = delete;

    [[nodiscard]] cc::read_write_stream stream() { return cc::read_write_stream(_buffer, _buffer, &impl_flush, this); }

private:
    static cc::result<i64>
    impl_flush(byte*& curr, byte*& end, byte*& write_end, void* ctx, i64 offset, cc::seek_dir dir, byte* /*first_write*/)
    {
        auto& self = *static_cast<mock_split_bounds_read_write_adapter*>(ctx);
        if (!(dir == cc::seek_dir::relative && offset == 0))
            return i64(-1); // not seekable

        curr = self._buffer;
        end = self._buffer + self._readable; // only the filled prefix is readable
        write_end = self._buffer + k_cap;    // ... but the whole buffer can be written
        return i64(-1);
    }

    isize _readable;
    byte _buffer[k_cap] = {};
};
} // namespace cc_stream_test
