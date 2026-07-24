#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // min/max, unit
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh> // read_all result buffer
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/fwd.hh>
#include <clean-core/streams/stream_flush.hh> // public: cc::seek_dir, cc::stream_flush_fn
#include <clean-core/string/string.hh>        // read_line output buffer

#include <cstring> // std::memcpy
#include <type_traits>

// Implementation core for the byte streams: the internal stream_access enum and the single
// cc::impl::stream<Access, Seekable> engine that the six public stream types in <clean-core/streams/stream.hh>
// are built from. The author-facing flush contract (cc::seek_dir, cc::stream_flush_fn) is public and lives in
// <clean-core/streams/stream_flush.hh>. See stream.hh for the concept and the performance/conversion rationale.

namespace cc::impl
{
/// Access capability of a stream.
enum class stream_access : cc::u8
{
    read,
    write,
    read_write,
};

/// Maps (access, seekable) to the concrete public stream type (cc::read_stream, ...). Specialized in
/// stream.hh; lets engine members like try_as_seekable name the public type instead of the bare engine.
template <stream_access Access, bool Seekable>
struct public_stream;

constexpr bool stream_can_read(stream_access a)
{
    return a == stream_access::read || a == stream_access::read_write;
}
constexpr bool stream_can_write(stream_access a)
{
    return a == stream_access::write || a == stream_access::read_write;
}

/// True if a stream of access `from` may narrow to access `to`. read_write drops to read or write; the leaf
/// levels read/write never convert into one another.
constexpr bool stream_access_narrows(stream_access from, stream_access to)
{
    return from == to || (from == stream_access::read_write && (to == stream_access::read || to == stream_access::write));
}

/// True if the public stream type `To` is `From` or a legal narrowing of it (drop seekable, read_write -> read
/// or write). Used by adapters to gate which stream types they can hand out. Both must expose `::engine`.
template <class From, class To>
concept stream_narrows_to = requires {
    typename From::engine;
    typename To::engine;
} && (!To::engine::can_read || From::engine::can_read) && (!To::engine::can_write || From::engine::can_write) && (!To::engine::is_seekable || From::engine::is_seekable);

constexpr bool seek_dir_is_dry(seek_dir d)
{
    return d == seek_dir::dry_begin || d == seek_dir::dry_relative || d == seek_dir::dry_end;
}

/// Empty placeholder occupying the `first_write` slot on read-only streams (zero-sized via
/// [[no_unique_address]]), so read_stream keeps the exact {curr, end, flush, context} layout.
struct stream_no_first_write
{
};

template <stream_access Access>
using stream_first_write_t = std::conditional_t<stream_can_write(Access), cc::byte*, stream_no_first_write>;

/// Present only on a read_write stream: the write-capacity end, distinct from `_end` (the read/valid end).
/// On every other stream the write bound is just `_end`, so this is zero-sized.
template <stream_access Access>
using stream_write_end_t
    = std::conditional_t<stream_can_read(Access) && stream_can_write(Access), cc::byte*, stream_no_first_write>;

/// The single templated stream engine; the six public stream types in <clean-core/streams/stream.hh> inherit
/// the matching instantiation and pull in its capability-relevant methods. Members/methods gated by params.
template <stream_access Access, bool Seekable>
struct stream
{
    static constexpr bool can_read = stream_can_read(Access);
    static constexpr bool can_write = stream_can_write(Access);
    static constexpr bool is_seekable = Seekable;

    // construction
public:
    /// An invalid (unbound) stream.
    stream() = default;

    /// Bind a stream to an adapter's window + flush callback. Called by adapters; `context` must outlive the
    /// stream. curr/end delimit the initial window (equal for a buffered adapter, full for a span).
    stream(cc::byte* curr, cc::byte* end, stream_flush_fn flush, void* context)
      : _curr(curr), _end(end), _flush(flush), _context(context)
    {
        // a read_write stream's write capacity starts at the initial window end; the first flush refines it
        if constexpr (can_read && can_write)
            _write_end = end;
    }

    // move-only, invalidating the source
    stream(stream const&) = delete;
    stream& operator=(stream const&) = delete;

    stream(stream&& other) noexcept { this->impl_take_from(other); }
    stream& operator=(stream&& other) noexcept
    {
        if (this != &other)
            this->impl_take_from(other);
        return *this;
    }
    ~stream() = default;

    // validity
public:
    /// True while the stream is bound to an adapter (false after a move consumed it).
    [[nodiscard]] bool is_valid() const { return _flush != nullptr; }

    // flushing (all streams)
public:
    /// Plain flush: refill the read window / write through pending bytes. Returns the position of curr if
    /// the source tracks one, else -1; a cc::result error on I/O failure.
    cc::result<cc::i64> flush() { return this->impl_flush(0, seek_dir::relative); }

    // reading
public:
    /// Bytes buffered and ready to read right now, without another flush: [curr, end).
    [[nodiscard]] cc::span<cc::byte const> ready_bytes() const
        requires(can_read)
    {
        CC_ASSERT(this->is_valid(), "ready_bytes on invalid stream");
        return cc::span<cc::byte const>(_curr, _end);
    }

    /// Consume n of the ready_bytes() (advance the read cursor). No I/O. Precondition: 0 <= n <= ready_bytes().size().
    void consume(cc::isize n)
        requires(can_read)
    {
        CC_ASSERT(this->is_valid(), "consume on invalid stream");
        CC_ASSERT(0 <= n && n <= _end - _curr, "consume past ready bytes");
        _curr += n;
    }

    /// True if there is no more data. On a seekable stream this is a dry check (position vs size — the buffer
    /// is not disturbed); on a non-seekable stream the window must actually be refilled to tell.
    cc::result<bool> at_end()
        requires(can_read)
    {
        CC_ASSERT(this->is_valid(), "at_end on invalid stream");
        if (_curr != _end)
            return false; // bytes still buffered
        if constexpr (is_seekable)
        {
            auto pos = this->impl_flush(0, seek_dir::dry_relative);
            CC_RETURN_IF_ERROR(pos);
            auto sz = this->impl_flush(0, seek_dir::dry_end);
            CC_RETURN_IF_ERROR(sz);
            return pos.value() >= sz.value();
        }
        else
        {
            CC_RETURN_IF_ERROR(this->flush()); // no size to query — must attempt a refill
            return _curr == _end;
        }
    }

    /// Read up to dst.size() bytes into dst, returning the count read (0 only at end-of-data). Refills the
    /// buffer as needed.
    cc::result<cc::isize> read(cc::span<cc::byte> dst)
        requires(can_read)
    {
        CC_ASSERT(this->is_valid(), "read on invalid stream");
        cc::isize total = 0;
        while (total < dst.size())
        {
            if (_curr == _end)
            {
                CC_RETURN_IF_ERROR(this->flush());
                if (_curr == _end)
                    break; // genuine end of data
            }
            auto const n = cc::min(dst.size() - total, cc::isize(_end - _curr));
            std::memcpy(dst.data() + total, _curr, size_t(n));
            _curr += n;
            total += n;
        }
        return total;
    }

    /// Read exactly dst.size() bytes into dst; errors if the stream ends short.
    cc::result<cc::unit> read_exact(cc::span<cc::byte> dst)
        requires(can_read)
    {
        auto n = this->read(dst);
        CC_RETURN_IF_ERROR(n);
        if (n.value() != dst.size())
            return cc::error("read_exact: unexpected end of stream");
        return cc::unit{};
    }

    /// Read one trivially-copyable value.
    template <class T>
    cc::result<T> read_pod()
        requires(can_read)
    {
        static_assert(std::is_trivially_copyable_v<T>, "read_pod needs a trivially-copyable T");
        T value;
        CC_RETURN_IF_ERROR(
            this->read_exact(cc::span<cc::byte>(reinterpret_cast<cc::byte*>(&value), cc::isize(sizeof(T)))));
        return value;
    }

    /// Read the whole remaining stream into one contiguous buffer, consuming it (the stream ends at EOF).
    ///
    /// DO NOT hand-roll this over ready_bytes/flush in user code.
    /// The stream is ALREADY buffered by its adapter, so a manual loop just adds a second, pointless user-space buffer.
    /// This pushes each ready window straight into the result and refills — there is never a double copy.
    /// When the source can report its remaining size, the buffer is sized exactly once up front and filled with stable appends, so the common case is a single precise allocation.
    /// That covers any seekable stream, and any non-seekable one whose adapter still tracks position (most real streams).
    cc::result<cc::vector<cc::byte>> read_all()
        requires(can_read)
    {
        CC_ASSERT(this->is_valid(), "read_all on invalid stream");

        auto out = cc::vector<cc::byte>();
        auto const hint = this->impl_remaining_hint(); // exact remaining bytes if the source can report them
        auto const precise = hint.has_value();
        if (precise)
            out.reserve_back(cc::isize(hint.value()));

        while (true)
        {
            auto window = this->ready_bytes();
            if (window.empty())
            {
                CC_RETURN_IF_ERROR(this->flush());
                window = this->ready_bytes();
                if (window.empty())
                    break; // genuine end of data
            }
            if (precise)
                out.push_back_range_stable(window); // capacity reserved exactly; no reallocation
            else
                out.push_back_range(window); // unknown size: grow as we go
            this->consume(window.size());
        }
        return out;
    }

    /// Read one line into `out` (cleared first), excluding the terminator.
    /// A trailing '\r' of a "\r\n" ending is stripped, so Unix and Windows endings both work.
    /// Returns true when a line was read, false at end-of-data with nothing left.
    /// The final line of input without a trailing newline is returned once (true), then the next call returns false.
    /// An empty line yields true with an empty `out`.
    ///
    /// `max_size` bounds the bytes appended to `out`; `cc::nullopt` (the default) reads a line in full, so a pathological newline-free input can grow `out` up to the whole stream.
    /// When set and a line is longer, the call returns true with `out.size() == *max_size`, and the rest of the line (including its newline) stays buffered for the next call.
    /// No data is lost, but a long line comes back in pieces.
    cc::result<bool> read_line(cc::string& out, cc::optional<cc::isize> max_size = cc::nullopt)
        requires(can_read)
    {
        CC_ASSERT(this->is_valid(), "read_line on invalid stream");
        CC_ASSERT(!max_size.has_value() || max_size.value() >= 0, "read_line: max_size must be non-negative");

        out.clear();
        auto read_any = false;
        while (true)
        {
            auto window = this->ready_bytes();
            if (window.empty())
            {
                CC_RETURN_IF_ERROR(this->flush());
                window = this->ready_bytes();
                if (window.empty())
                    return read_any; // genuine end of data
            }
            read_any = true;

            auto const* const base = reinterpret_cast<char const*>(window.data());
            auto newline = cc::isize(-1);
            for (auto i = cc::isize(0); i < window.size(); ++i)
                if (base[i] == '\n')
                {
                    newline = i;
                    break;
                }

            // How many bytes of this window we may still append before hitting max_size.
            auto const budget = max_size.has_value() ? max_size.value() - out.size() : cc::isize(-1);
            auto const capped = max_size.has_value() && budget <= (newline >= 0 ? newline : window.size());
            if (capped)
            {
                // Fill exactly to the cap and stop; leave the rest of the line (and its '\n') for next time.
                out += cc::string_view(base, budget);
                this->consume(budget);
                return true;
            }

            if (newline >= 0)
            {
                out += cc::string_view(base, newline);
                this->consume(newline + 1); // consume the line and its '\n'
                if (!out.empty() && out[out.size() - 1] == '\r')
                    out.resize_down_to(out.size() - 1); // strip the '\r' of a "\r\n" ending (may split across windows)
                return true;
            }

            // no newline in this window: take all of it and refill
            out += cc::string_view(base, window.size());
            this->consume(window.size());
        }
    }

    // writing
public:
    /// The free buffer space you can write into right now, before a flush: [curr, write_end). Write into it,
    /// then produce(n). (This is NOT a flush — no I/O; it just exposes the buffer.)
    [[nodiscard]] cc::span<cc::byte> writable_bytes() const
        requires(can_write)
    {
        CC_ASSERT(this->is_valid(), "writable_bytes on invalid stream");
        return cc::span<cc::byte>(_curr, this->impl_write_bound());
    }

    /// Mark n bytes (written into writable_bytes()) as produced (advance the write cursor). No I/O.
    /// Precondition: 0 <= n <= writable_bytes().size().
    void produce(cc::isize n)
        requires(can_write)
    {
        CC_ASSERT(this->is_valid(), "produce on invalid stream");
        CC_ASSERT(0 <= n && n <= this->impl_write_bound() - _curr, "produce past writable bytes");
        if (_first_write == nullptr)
            _first_write = _curr;
        _curr += n;
    }

    /// Write all bytes of src, draining as needed. Errors if a bounded sink runs out of space.
    cc::result<cc::unit> write(cc::span<cc::byte const> src)
        requires(can_write)
    {
        CC_ASSERT(this->is_valid(), "write on invalid stream");
        cc::isize total = 0;
        while (total < src.size())
        {
            if (_curr == this->impl_write_bound())
            {
                CC_RETURN_IF_ERROR(this->flush());
                if (_curr == this->impl_write_bound())
                    return cc::error("write: sink is full");
            }
            auto const n = cc::min(src.size() - total, cc::isize(this->impl_write_bound() - _curr));
            if (_first_write == nullptr)
                _first_write = _curr;
            std::memcpy(_curr, src.data() + total, size_t(n));
            _curr += n;
            total += n;
        }
        return cc::unit{};
    }

    /// Write the bytes of one trivially-copyable value.
    template <class T>
    cc::result<cc::unit> write_pod(T const& value)
        requires(can_write)
    {
        static_assert(std::is_trivially_copyable_v<T>, "write_pod needs a trivially-copyable T");
        return this->write(cc::span<T const>(&value, 1).as_bytes());
    }

    // seeking (seekable streams only; O(1) or O(log n) by contract)
public:
    /// Seek to an absolute byte offset from the start. Returns the new position.
    cc::result<cc::i64> seek_to(cc::i64 offset)
        requires(is_seekable)
    {
        return this->impl_flush(offset, seek_dir::begin);
    }

    /// Seek by a signed delta from the current position. Returns the new position.
    cc::result<cc::i64> skip(cc::i64 delta)
        requires(is_seekable)
    {
        return this->impl_flush(delta, seek_dir::relative);
    }

    /// Seek to `offset` bytes relative to the end (offset <= 0 addresses within the data). Returns the new
    /// position.
    cc::result<cc::i64> seek_from_end(cc::i64 offset)
        requires(is_seekable)
    {
        return this->impl_flush(offset, seek_dir::end);
    }

    /// Current absolute position (bytes from start). Does not disturb the buffer.
    cc::result<cc::i64> position()
        requires(is_seekable)
    {
        return this->impl_flush(0, seek_dir::dry_relative);
    }

    /// Total size of the underlying data. Does not disturb the buffer.
    cc::result<cc::i64> size()
        requires(is_seekable)
    {
        return this->impl_flush(0, seek_dir::dry_end);
    }

    /// Bytes between the current position and the end. Does not disturb the buffer.
    cc::result<cc::i64> remaining_bytes()
        requires(is_seekable)
    {
        auto p = this->impl_flush(0, seek_dir::dry_relative);
        CC_RETURN_IF_ERROR(p);
        auto e = this->impl_flush(0, seek_dir::dry_end);
        CC_RETURN_IF_ERROR(e);
        return e.value() - p.value();
    }

    // seekability upgrade
public:
    /// Try to upgrade a non-seekable stream to its seekable variant in place. On success the seekable stream
    /// is returned and *this becomes invalid; on failure returns nullopt and *this stays valid. Uses a dry
    /// probe, so the buffer is never disturbed either way.
    cc::optional<typename public_stream<Access, true>::type> try_as_seekable() &&
        requires(!is_seekable)
    {
        CC_ASSERT(this->is_valid(), "try_as_seekable on invalid stream");
        auto const probe = this->impl_flush(0, seek_dir::dry_relative);
        if (probe.has_error() || probe.value() < 0)
            return cc::nullopt;

        typename public_stream<Access, true>::type result;
        result._curr = _curr;
        result._end = _end;
        result._flush = _flush;
        result._context = _context;
        if constexpr (can_write)
            result._first_write = _first_write;
        if constexpr (can_read && can_write)
            result._write_end = _write_end;
        this->impl_invalidate();
        return result;
    }

    // implementation
private:
    template <stream_access, bool>
    friend struct stream;

    // The write boundary: the separate _write_end for a read_write stream, otherwise _end itself (writes on a
    // write-only stream already use _end as their capacity). Two overloads so flush can update it by reference.
    [[nodiscard]] cc::byte* impl_write_bound() const
    {
        if constexpr (can_read && can_write)
            return _write_end;
        else
            return _end;
    }
    [[nodiscard]] cc::byte*& impl_write_bound_ref()
    {
        if constexpr (can_read && can_write)
            return _write_end;
        else
            return _end;
    }

    /// Exact remaining byte count if the source can report it cheaply, else nullopt.
    /// Never disturbs the buffer — dry probes only.
    /// A dry_relative probe is safe on any stream (try_as_seekable uses the same one to detect seekability).
    /// A real position (>= 0) means the source tracks position, so the dry_end probe that follows is safe too.
    /// A genuinely non-seekable source returns -1, and we bail.
    cc::optional<cc::i64> impl_remaining_hint()
        requires(can_read)
    {
        auto const pos = this->impl_flush(0, seek_dir::dry_relative);
        if (pos.has_error() || pos.value() < 0)
            return cc::nullopt;
        auto const end = this->impl_flush(0, seek_dir::dry_end);
        if (end.has_error() || end.value() < 0)
            return cc::nullopt;
        return end.value() - pos.value();
    }

    cc::result<cc::i64> impl_flush(cc::i64 offset, seek_dir dir)
    {
        CC_ASSERT(this->is_valid(), "flush on invalid stream");
        cc::byte* fw = nullptr;
        if constexpr (can_write)
            fw = _first_write;

        // write_end aliases _end unless this is a read_write stream (see impl_write_bound_ref)
        auto r = _flush(_curr, _end, this->impl_write_bound_ref(), _context, offset, dir, fw);

        if constexpr (can_write)
        {
            // pending writes were flushed through on any successful non-dry op; clear the marker (contract 5).
            // on error, leave it intact so the caller can retry.
            if (r.has_value() && !seek_dir_is_dry(dir))
                _first_write = nullptr;
        }
        return r;
    }

    void impl_take_from(stream& other)
    {
        _curr = other._curr;
        _end = other._end;
        _flush = other._flush;
        _context = other._context;
        if constexpr (can_write)
            _first_write = other._first_write;
        if constexpr (can_read && can_write)
            _write_end = other._write_end;
        other.impl_invalidate();
    }

    void impl_invalidate()
    {
        _curr = nullptr;
        _end = nullptr;
        _flush = nullptr;
        _context = nullptr;
        if constexpr (can_write)
            _first_write = nullptr;
        if constexpr (can_read && can_write)
            _write_end = nullptr;
    }

    // members
private:
    cc::byte* _curr = nullptr;
    cc::byte* _end = nullptr;
    stream_flush_fn _flush = nullptr;
    void* _context = nullptr;
    [[no_unique_address]] stream_first_write_t<Access> _first_write = {};
    [[no_unique_address]] stream_write_end_t<Access> _write_end = {};
};
} // namespace cc::impl
