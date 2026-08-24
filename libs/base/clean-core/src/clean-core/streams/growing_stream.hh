#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <clean-core/streams/stream.hh>
#include <clean-core/string/string.hh>

#include <type_traits>

// =========================================================================================================
// Growing in-memory write adapters (over a cc::vector<byte> / cc::string)
// =========================================================================================================
//
// The counterpart to span_stream.hh's BOUNDED in-memory sink: these own their buffer and grow it on demand, so a
// writer that does not know its output size up front has somewhere to write.
// Take the finished bytes with take(), which moves the container out.
//
// THE WINDOW IS THE CONTAINER'S SPARE CAPACITY, so bytes land in their final home and a flush copies nothing:
//
//     [data(), data() + size())              already committed
//     [data() + size(), data() + capacity)   the stream's [curr, end) window
//
// A flush commits the pending bytes by resizing the container over them (they are already stored there), then
// reserves more room and re-points curr/end, which is why the container may reallocate during a flush.
// Growth goes through reserve_back, so it is exponential and the cost per byte is amortized O(1).
//
// The streams are seekable: everything is in memory, so seeking back to patch an earlier byte — a length prefix
// written before its payload was known — costs nothing.
// Writing at a position before the committed end overwrites in place and may extend past it.
//
// The adapter is the stream's context, so it must OUTLIVE any stream taken from it.
// It is neither movable nor copyable, since moving it would leave that context dangling.
// Take one stream per adapter.
//
//   auto adapter = cc::string_write_stream_adapter();
//   auto stream = adapter.stream();
//   stream.write(cc::as_byte_span(cc::string_view("hello")));
//   CC_ASSERT(stream.flush().has_value());  // take() sees only what has been flushed
//   auto text = adapter.take();             // "hello"

namespace cc::impl
{
/// Flush for the growing adapters: commits pending bytes into `container`, grows it, and re-points the window.
/// `ctx` is the container itself.
/// Instantiated in growing_stream.cc for the two container types below.
template <class Container>
cc::result<i64>
growing_adapter_flush(byte*& curr, byte*& end, byte*& write_end, void* ctx, i64 offset, seek_dir dir, byte* first_write);

extern template cc::result<i64> growing_adapter_flush<cc::vector<byte>>(byte*&, byte*&, byte*&, void*, i64, seek_dir, byte*);
extern template cc::result<i64> growing_adapter_flush<cc::string>(byte*&, byte*&, byte*&, void*, i64, seek_dir, byte*);

/// Shared base for the growing adapters: owns the container and hands out a seekable_write_stream over its spare capacity.
template <class Container>
class growing_adapter_base
{
public:
    growing_adapter_base(growing_adapter_base&&) = delete;
    growing_adapter_base& operator=(growing_adapter_base&&) = delete;
    growing_adapter_base(growing_adapter_base const&) = delete;
    growing_adapter_base& operator=(growing_adapter_base const&) = delete;

    /// The natural (most-capable) stream over the buffer.
    [[nodiscard]] cc::seekable_write_stream stream() { return this->impl_make<cc::seekable_write_stream>(); }

    /// Convert straight to the natural stream or any narrower one, so the adapter drops into a function expecting a stream.
    template <class Stream>
        requires stream_narrows_to<cc::seekable_write_stream, Stream>
    operator Stream()
    {
        return this->impl_make<Stream>();
    }

    /// Moves the finished buffer out, leaving the adapter empty.
    /// Only bytes the stream has flushed are included, so flush (or destroy) the stream first.
    [[nodiscard]] Container take() { return cc::move(_data); }

protected:
    explicit growing_adapter_base(Container initial) : _data(cc::move(initial)) {}

private:
    template <class Stream>
    [[nodiscard]] Stream impl_make()
    {
        auto* const base = reinterpret_cast<byte*>(_data.data());
        auto* const curr = base + _data.size();
        return Stream(curr, curr + _data.capacity_back(), &growing_adapter_flush<Container>, &_data);
    }

    Container _data;
};
} // namespace cc::impl

/// Write adapter over an owned, growing cc::vector<byte>, handing out a seekable_write_stream.
/// Unbounded: a write never fails for lack of space.
class cc::vector_write_stream_adapter : public impl::growing_adapter_base<cc::vector<byte>>
{
public:
    /// Starts empty, or appends after the content of `initial`.
    explicit vector_write_stream_adapter(cc::vector<byte> initial = {}) : growing_adapter_base(cc::move(initial)) {}
};

/// Write adapter over an owned, growing cc::string, handing out a seekable_write_stream.
/// The natural sink for a text format: take() hands back the cc::string directly, with no copy.
class cc::string_write_stream_adapter : public impl::growing_adapter_base<cc::string>
{
public:
    /// Starts empty, or appends after the content of `initial`.
    explicit string_write_stream_adapter(cc::string initial = {}) : growing_adapter_base(cc::move(initial)) {}
};
