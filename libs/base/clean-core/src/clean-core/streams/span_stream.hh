#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <clean-core/fwd.hh>
#include <clean-core/streams/stream.hh>

#include <type_traits>

// =========================================================================================================
// In-memory stream adapters (over a cc::span)
// =========================================================================================================
//
// A span adapter is UNBUFFERED: the whole span fits directly into the stream's [curr, end), so no refill ever
// happens and the streams are inherently seekable (O(1)). The adapter owns only the bounds and serves as the
// stream's context — it must outlive any stream taken from it (the stream stores a pointer into it). Take one
// stream per adapter.
//
// Construct explicitly, e.g. `cc::span_read_stream_adapter(bytes)`; the adapter then converts implicitly to the
// matching stream (or any legal narrowing of it), so it can be passed straight to a function expecting a
// stream. `.stream()` returns the natural seekable stream explicitly.
//
// The three adapters differ only in span const-ness and which stream they produce; they share all state and
// logic through impl::span_adapter_base. The single (type-erased) flush lives in span_stream.cc.

namespace cc::impl
{
/// The entire state a span adapter needs: the span bounds. Referenced by the flush callback via `void* ctx`.
struct span_adapter_state
{
    cc::byte* base;
    cc::isize size;
};

/// Flush for every span adapter: the span is fully in memory, so a plain flush is a no-op that just restores
/// end = base + size; seeks reposition curr within [base, base + size]; the span is always seekable. Write-
/// through is a no-op because bytes are written directly into the destination span (curr == end means the
/// bounded sink is full). Returns the position of curr, or an error on an out-of-range seek.
cc::result<cc::i64> span_adapter_flush(cc::byte*& curr,
                                       cc::byte*& end,
                                       cc::byte*& write_end,
                                       void* ctx,
                                       cc::i64 offset,
                                       seek_dir dir,
                                       cc::byte* first_write);

/// Shared base for the span adapters: holds the bounds and hands out `NaturalStream` (a seekable_* stream).
template <class NaturalStream>
class span_adapter_base
{
public:
    span_adapter_base(span_adapter_base&&) noexcept = default;
    span_adapter_base& operator=(span_adapter_base&&) noexcept = default;
    span_adapter_base(span_adapter_base const&) = delete;
    span_adapter_base& operator=(span_adapter_base const&) = delete;

    /// The natural (most-capable) stream over the span.
    [[nodiscard]] NaturalStream stream() { return this->impl_make<NaturalStream>(); }

    /// Convert straight to the natural stream or any narrower one (e.g. a span_read_write_stream_adapter to a plain
    /// read_stream), so the adapter drops into a function expecting a stream.
    template <class Stream>
        requires stream_narrows_to<NaturalStream, Stream>
    operator Stream()
    {
        return this->impl_make<Stream>();
    }

protected:
    span_adapter_base(cc::byte* base, cc::isize size) : _state{base, size} {}

private:
    template <class Stream>
    [[nodiscard]] Stream impl_make()
    {
        return Stream(_state.base, _state.base + _state.size, &span_adapter_flush, &_state);
    }

    span_adapter_state _state;
};
} // namespace cc::impl

namespace cc
{
/// Read adapter over an immutable byte span. Hands out a seekable_read_stream.
class span_read_stream_adapter : public impl::span_adapter_base<seekable_read_stream>
{
public:
    explicit span_read_stream_adapter(cc::span<cc::byte const> data)
      // stored non-const, but the read path never writes through it
      : span_adapter_base(const_cast<cc::byte*>(data.data()), data.size())
    {
    }
};

/// Write adapter over a mutable byte span. Hands out a seekable_write_stream. Bounded: the sink is full once
/// curr == end, and writing past it errors.
class span_write_stream_adapter : public impl::span_adapter_base<seekable_write_stream>
{
public:
    explicit span_write_stream_adapter(cc::span<cc::byte> data) : span_adapter_base(data.data(), data.size()) {}
};

/// Read+write adapter over a mutable byte span. Hands out a seekable_read_write_stream; reads and writes share
/// the single cursor within [base, base + size].
class span_read_write_stream_adapter : public impl::span_adapter_base<seekable_read_write_stream>
{
public:
    explicit span_read_write_stream_adapter(cc::span<cc::byte> data) : span_adapter_base(data.data(), data.size()) {}
};
} // namespace cc
