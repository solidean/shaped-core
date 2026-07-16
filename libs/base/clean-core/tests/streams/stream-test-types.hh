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
inline cc::byte b(int v)
{
    return cc::byte(v);
}

inline bool bytes_equal(cc::span<cc::byte const> a, cc::span<cc::byte const> b)
{
    if (a.size() != b.size())
        return false;
    return a.empty() || std::memcmp(a.data(), b.data(), size_t(a.size())) == 0;
}

/// A non-seekable in-memory READ source (models a pipe): it serves bytes from a fixed source in fixed-size
/// chunks and returns -1 for every seek / dry-seek, so try_as_seekable must fail on it. A plain flush
/// (relative, 0) still works and also returns -1 (no meaningful position).
class mock_pipe_read_stream_adapter
{
public:
    mock_pipe_read_stream_adapter(cc::span<cc::byte const> data, cc::isize chunk) : _data(data), _chunk(chunk) {}

    mock_pipe_read_stream_adapter(mock_pipe_read_stream_adapter&&) = delete; // pinned: the stream borrows _buffer
    mock_pipe_read_stream_adapter& operator=(mock_pipe_read_stream_adapter&&) = delete;

    [[nodiscard]] cc::read_stream stream() { return cc::read_stream(_buffer, _buffer, &impl_flush, this); }

private:
    static cc::result<cc::i64> impl_flush(cc::byte*& curr,
                                          cc::byte*& end,
                                          cc::byte*& /*write_end*/, // aliases end for a read-only stream
                                          void* ctx,
                                          cc::i64 offset,
                                          cc::seek_dir dir,
                                          cc::byte* /*first_write*/)
    {
        auto& self = *static_cast<mock_pipe_read_stream_adapter*>(ctx);
        if (!(dir == cc::seek_dir::relative && offset == 0))
            return cc::i64(-1); // not seekable

        cc::byte* const base = self._buffer;
        cc::isize const leftover = cc::isize(end - curr);
        std::memmove(base, curr, size_t(leftover));

        cc::isize const room = cc::isize(sizeof(self._buffer)) - leftover;
        cc::isize const want = cc::min(self._chunk, room);
        cc::isize const avail = self._data.size() - self._pos;
        cc::isize const n = cc::min(want, avail);
        if (n > 0)
            std::memcpy(base + leftover, self._data.data() + self._pos, size_t(n));
        self._pos += n;

        curr = base;
        end = base + leftover + n;
        return cc::i64(-1); // a pipe has no meaningful position
    }

    cc::span<cc::byte const> _data;
    cc::isize _chunk;
    cc::isize _pos = 0;
    cc::byte _buffer[64];
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

    [[nodiscard]] cc::span<cc::byte const> written() const { return _sink; }
    [[nodiscard]] int flushes_with_pending() const { return _flushes_with_pending; }

private:
    static constexpr cc::isize k_cap = 64;

    static cc::result<cc::i64> impl_flush(cc::byte*& curr,
                                          cc::byte*& end,
                                          cc::byte*& /*write_end*/, // aliases end for a write-only stream
                                          void* ctx,
                                          cc::i64 offset,
                                          cc::seek_dir dir,
                                          cc::byte* first_write)
    {
        auto& self = *static_cast<recording_write_stream_adapter*>(ctx);
        if (!(dir == cc::seek_dir::relative && offset == 0))
            return cc::i64(-1); // not seekable

        if (first_write != nullptr && curr > first_write)
        {
            for (cc::byte* p = first_write; p != curr; ++p)
                self._sink.push_back(*p);
            ++self._flushes_with_pending;
        }
        curr = self._buffer;
        end = self._buffer + k_cap;
        return cc::i64(-1);
    }

    cc::vector<cc::byte> _sink;
    int _flushes_with_pending = 0;
    cc::byte _buffer[k_cap];
};
} // namespace cc_stream_test
