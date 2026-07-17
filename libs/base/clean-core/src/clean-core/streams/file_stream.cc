#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::move, cc::max
#include <clean-core/streams/file_stream.hh>

#include <cstring> // std::memmove

namespace cc
{
// =========================================================================================================
// file_read_stream_adapter
// =========================================================================================================

cc::result<file_read_stream_adapter> file_read_stream_adapter::open(cc::string_view path)
{
    auto f = cc::impl::native_file::open(path, cc::impl::file_mode::read);
    CC_RETURN_IF_ERROR(f);

    file_read_stream_adapter p;
    p._file = cc::move(f.value());
    return p;
}

cc::result<cc::i64> file_read_stream_adapter::impl_seek_and_fill(cc::byte*& curr, cc::byte*& end, cc::i64 target)
{
    if (target < 0)
        return cc::error("file stream: seek before start");

    auto s = _file.seek(target);
    CC_RETURN_IF_ERROR(s);
    _buffer_offset = target;

    auto n = _file.read(cc::span<cc::byte>(_buffer, k_buffer_size));
    CC_RETURN_IF_ERROR(n);
    curr = _buffer;
    end = _buffer + n.value();
    return target;
}

cc::result<cc::i64> file_read_stream_adapter::impl_flush(cc::byte*& curr,
                                                         cc::byte*& end,
                                                         cc::byte*& /*write_end*/, // aliases end for a read-only stream
                                                         void* ctx,
                                                         cc::i64 offset,
                                                         cc::seek_dir dir,
                                                         cc::byte* /*first_write*/)
{
    using sd = cc::seek_dir;
    auto& self = *static_cast<file_read_stream_adapter*>(ctx);
    cc::byte* const base = self._buffer;
    cc::i64 const pos = self._buffer_offset + cc::i64(curr - base);

    switch (dir)
    {
    case sd::relative:
        if (offset == 0)
        {
            // plain refill: preserve the leftover [curr, end), then fill the remainder
            cc::isize const leftover = cc::isize(end - curr);
            CC_ASSERT(leftover < file_read_stream_adapter::k_buffer_size, "refilling a full buffer makes no progress");
            std::memmove(base, curr, size_t(leftover));
            self._buffer_offset = pos;
            auto n = self._file.read(
                cc::span<cc::byte>(base + leftover, file_read_stream_adapter::k_buffer_size - leftover));
            CC_RETURN_IF_ERROR(n);
            curr = base;
            end = base + leftover + n.value();
            return self._buffer_offset; // == pos; curr == end iff end of file
        }
        return self.impl_seek_and_fill(curr, end, pos + offset);

    case sd::begin:
        return self.impl_seek_and_fill(curr, end, offset);

    case sd::end:
    {
        auto sz = self._file.size();
        CC_RETURN_IF_ERROR(sz);
        return self.impl_seek_and_fill(curr, end, sz.value() + offset);
    }

    case sd::dry_relative:
        return pos + offset;
    case sd::dry_begin:
        return offset;
    case sd::dry_end:
    {
        auto sz = self._file.size();
        CC_RETURN_IF_ERROR(sz);
        return sz.value() + offset;
    }
    }
    CC_UNREACHABLE("invalid seek_dir");
}

// =========================================================================================================
// file_write_stream_adapter
// =========================================================================================================

cc::result<file_write_stream_adapter> file_write_stream_adapter::create(cc::string_view path)
{
    auto f = cc::impl::native_file::open(path, cc::impl::file_mode::write_truncate);
    CC_RETURN_IF_ERROR(f);

    file_write_stream_adapter p;
    p._file = cc::move(f.value());
    return p;
}

cc::result<file_write_stream_adapter> file_write_stream_adapter::open(cc::string_view path)
{
    auto f = cc::impl::native_file::open(path, cc::impl::file_mode::write_keep);
    CC_RETURN_IF_ERROR(f);

    file_write_stream_adapter p;
    p._file = cc::move(f.value());
    return p; // _buffer_offset stays 0: overwrite from the start
}

cc::result<file_write_stream_adapter> file_write_stream_adapter::append(cc::string_view path)
{
    auto f = cc::impl::native_file::open(path, cc::impl::file_mode::write_keep);
    CC_RETURN_IF_ERROR(f);
    auto sz = f.value().size();
    CC_RETURN_IF_ERROR(sz);

    file_write_stream_adapter p;
    p._file = cc::move(f.value());
    p._buffer_offset = sz.value(); // start the window at the current end of the file
    return p;
}

cc::result<cc::i64> file_write_stream_adapter::impl_write_through(cc::byte*& curr,
                                                                  cc::byte*& end,
                                                                  cc::byte* first_write,
                                                                  cc::i64 pos)
{
    if (first_write != nullptr && curr > first_write)
    {
        cc::i64 const off = _buffer_offset + cc::i64(first_write - _buffer);
        auto s = _file.seek(off);
        CC_RETURN_IF_ERROR(s);

        cc::byte const* p = first_write;
        cc::isize remaining = cc::isize(curr - first_write);
        while (remaining > 0)
        {
            auto w = _file.write(cc::span<cc::byte const>(p, remaining));
            CC_RETURN_IF_ERROR(w);
            CC_ASSERT(w.value() > 0, "write made no progress");
            p += w.value();
            remaining -= w.value();
        }
    }

    // the whole buffer is free again, now positioned at the logical write cursor
    _buffer_offset = pos;
    curr = _buffer;
    end = _buffer + k_buffer_size;
    return pos;
}

cc::result<cc::i64> file_write_stream_adapter::impl_reposition(cc::byte*& curr, cc::byte*& end, cc::i64 target)
{
    if (target < 0)
        return cc::error("file stream: seek before start");

    auto s = _file.seek(target);
    CC_RETURN_IF_ERROR(s);
    _buffer_offset = target;
    curr = _buffer;
    end = _buffer + k_buffer_size;
    return target;
}

cc::result<cc::i64> file_write_stream_adapter::impl_flush(cc::byte*& curr,
                                                          cc::byte*& end,
                                                          cc::byte*& /*write_end*/, // aliases end for a write-only stream
                                                          void* ctx,
                                                          cc::i64 offset,
                                                          cc::seek_dir dir,
                                                          cc::byte* first_write)
{
    using sd = cc::seek_dir;
    auto& self = *static_cast<file_write_stream_adapter*>(ctx);
    cc::byte* const base = self._buffer;
    cc::i64 const pos = self._buffer_offset + cc::i64(curr - base); // total bytes written logically

    switch (dir)
    {
    case sd::relative:
        // drain pending writes first, then reposition (a plain flush is just the drain)
        CC_RETURN_IF_ERROR(self.impl_write_through(curr, end, first_write, pos));
        if (offset == 0)
            return self._buffer_offset; // == pos
        return self.impl_reposition(curr, end, pos + offset);

    case sd::begin:
        CC_RETURN_IF_ERROR(self.impl_write_through(curr, end, first_write, pos));
        return self.impl_reposition(curr, end, offset);

    case sd::end:
    {
        CC_RETURN_IF_ERROR(self.impl_write_through(curr, end, first_write, pos));
        auto sz = self._file.size();
        CC_RETURN_IF_ERROR(sz);
        return self.impl_reposition(curr, end, cc::max(sz.value(), pos) + offset);
    }

    case sd::dry_relative:
        return pos + offset;
    case sd::dry_begin:
        return offset;
    case sd::dry_end:
    {
        auto sz = self._file.size();
        CC_RETURN_IF_ERROR(sz);
        return cc::max(sz.value(), pos) + offset;
    }
    }
    CC_UNREACHABLE("invalid seek_dir");
}

// =========================================================================================================
// file_read_write_stream_adapter
// =========================================================================================================

cc::result<file_read_write_stream_adapter> file_read_write_stream_adapter::open(cc::string_view path)
{
    auto f = cc::impl::native_file::open(path, cc::impl::file_mode::read_write);
    CC_RETURN_IF_ERROR(f);

    file_read_write_stream_adapter p;
    p._file = cc::move(f.value());
    return p;
}

cc::result<cc::i64> file_read_write_stream_adapter::impl_drain(cc::byte* curr, cc::byte* first_write)
{
    // persist the pending writes [first_write, curr) at their file offset (grows the file if past the end)
    if (first_write != nullptr && curr > first_write)
    {
        cc::i64 const off = _buffer_offset + cc::i64(first_write - _buffer);
        auto s = _file.seek(off);
        CC_RETURN_IF_ERROR(s);

        cc::byte const* p = first_write;
        cc::isize remaining = cc::isize(curr - first_write);
        while (remaining > 0)
        {
            auto w = _file.write(cc::span<cc::byte const>(p, remaining));
            CC_RETURN_IF_ERROR(w);
            CC_ASSERT(w.value() > 0, "write made no progress");
            p += w.value();
            remaining -= w.value();
        }
    }
    return cc::i64(0);
}

cc::result<cc::i64> file_read_write_stream_adapter::impl_fill(cc::byte*& curr,
                                                              cc::byte*& end,
                                                              cc::byte*& write_end,
                                                              cc::isize leftover)
{
    cc::byte* const base = _buffer;
    // the unconsumed read data occupies [base, base + leftover); fresh file data follows it on disk
    auto s = _file.seek(_buffer_offset + leftover);
    CC_RETURN_IF_ERROR(s);
    auto n = _file.read(cc::span<cc::byte>(base + leftover, k_buffer_size - leftover));
    CC_RETURN_IF_ERROR(n);

    curr = base;
    end = base + leftover + n.value(); // read boundary: end of valid file data
    write_end = base + k_buffer_size;  // write capacity: the whole buffer (writing past `end` grows the file)
    return _buffer_offset;             // position of curr == _buffer_offset
}

cc::result<cc::i64> file_read_write_stream_adapter::impl_seek_and_fill(cc::byte*& curr,
                                                                       cc::byte*& end,
                                                                       cc::byte*& write_end,
                                                                       cc::i64 target)
{
    if (target < 0)
        return cc::error("file stream: seek before start");

    _buffer_offset = target;
    return this->impl_fill(curr, end, write_end, 0);
}

cc::result<cc::i64> file_read_write_stream_adapter::impl_flush(cc::byte*& curr,
                                                               cc::byte*& end,
                                                               cc::byte*& write_end,
                                                               void* ctx,
                                                               cc::i64 offset,
                                                               cc::seek_dir dir,
                                                               cc::byte* first_write)
{
    using sd = cc::seek_dir;
    auto& self = *static_cast<file_read_write_stream_adapter*>(ctx);
    cc::byte* const base = self._buffer;
    cc::i64 const pos = self._buffer_offset + cc::i64(curr - base);

    switch (dir)
    {
    case sd::relative:
        if (offset == 0)
        {
            // plain flush: persist pending writes, keep any unconsumed read leftover, refill the remainder.
            // curr may sit past `end` (wrote beyond valid data), so clamp the leftover at 0.
            cc::isize const leftover = cc::max(cc::isize(0), cc::isize(end - curr));
            CC_RETURN_IF_ERROR(self.impl_drain(curr, first_write));
            std::memmove(base, curr, size_t(leftover));
            self._buffer_offset = pos;
            return self.impl_fill(curr, end, write_end, leftover);
        }
        CC_RETURN_IF_ERROR(self.impl_drain(curr, first_write));
        return self.impl_seek_and_fill(curr, end, write_end, pos + offset);

    case sd::begin:
        CC_RETURN_IF_ERROR(self.impl_drain(curr, first_write));
        return self.impl_seek_and_fill(curr, end, write_end, offset);

    case sd::end:
    {
        CC_RETURN_IF_ERROR(self.impl_drain(curr, first_write));
        auto sz = self._file.size();
        CC_RETURN_IF_ERROR(sz);
        return self.impl_seek_and_fill(curr, end, write_end, cc::max(sz.value(), pos) + offset);
    }

    case sd::dry_relative:
        return pos + offset;
    case sd::dry_begin:
        return offset;
    case sd::dry_end:
    {
        auto sz = self._file.size();
        CC_RETURN_IF_ERROR(sz);
        return cc::max(sz.value(), pos) + offset; // include buffered growth past the on-disk size
    }
    }
    CC_UNREACHABLE("invalid seek_dir");
}
} // namespace cc
