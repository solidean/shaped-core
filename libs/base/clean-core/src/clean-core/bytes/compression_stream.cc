#include "compression_stream.hh"

#include <clean-core/bytes/impl/compression_backend.hh>
#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::memmove
#include <clean-core/string/format.hh>

using namespace cc::primitive_defines;

namespace
{
using sd = cc::seek_dir;

/// Streaming needs a wrapper to resume into, which `raw` framing is the removal of.
/// Deflate's `zlib` framing has one — a header and a trailing Adler-32 — so it streams exactly as `frame` does.
[[nodiscard]] bool is_streamable(cc::compression_framing framing)
{
    return framing != cc::compression_framing::raw;
}
} // namespace

// --- decompressing read -----------------------------------------------------------------------------------

cc::result<cc::decompressing_read_stream_adapter> cc::decompressing_read_stream_adapter::create(cc::read_stream&& inner,
                                                                                                decompression_config cfg)
{
    CC_RETURN_IF_ERROR(impl::validate_decompression_config(cfg));

    if (!is_streamable(cfg.framing))
        return cc::error("decompressing stream: raw framing carries no wrapper to stream, so it cannot be read this "
                         "way");

    // The algorithm has to be settled before a context exists, and nothing has been read yet to sniff — create() does
    // not touch the inner stream.
    // So a decompressing stream must be told which algorithm it is reading.
    if (!cfg.algorithm.has_value())
        return cc::error("decompressing stream: the algorithm must be given explicitly, since nothing has been read "
                         "yet to sniff");

    auto adapter = decompressing_read_stream_adapter();
    adapter._inner = cc::move(inner);
    adapter._config = cfg;
    adapter._algorithm = cfg.algorithm.value();
    adapter._state = impl::backend_for(adapter._algorithm).create_stream_decompressor(cfg);

    if (adapter._state == nullptr)
        return cc::error("decompressing stream: the backend refused to create a decompression context");

    return adapter;
}

cc::decompressing_read_stream_adapter::~decompressing_read_stream_adapter()
{
    if (_state != nullptr)
        impl::backend_for(_algorithm).destroy_stream_decompressor(_state);
}

// _state is an owning raw pointer, so the move has to clear the source: a defaulted move would leave both objects
// destroying the same context.
cc::decompressing_read_stream_adapter::decompressing_read_stream_adapter(decompressing_read_stream_adapter&& rhs) noexcept
  : _inner(cc::move(rhs._inner)),
    _config(rhs._config),
    _algorithm(rhs._algorithm),
    _state(rhs._state),
    _remaining_hint(rhs._remaining_hint),
    _total_produced(rhs._total_produced),
    _hint_probed(rhs._hint_probed),
    _frame_finished(rhs._frame_finished)
{
    cc::memcpy(_buffer, rhs._buffer, size_t(k_buffer_size));
    rhs._state = nullptr;
}

cc::decompressing_read_stream_adapter& cc::decompressing_read_stream_adapter::operator=(
    decompressing_read_stream_adapter&& rhs) noexcept
{
    if (this == &rhs)
        return *this;

    if (_state != nullptr)
        impl::backend_for(_algorithm).destroy_stream_decompressor(_state);

    _inner = cc::move(rhs._inner);
    _config = rhs._config;
    _algorithm = rhs._algorithm;
    _state = rhs._state;
    _remaining_hint = rhs._remaining_hint;
    _total_produced = rhs._total_produced;
    _hint_probed = rhs._hint_probed;
    _frame_finished = rhs._frame_finished;
    cc::memcpy(_buffer, rhs._buffer, size_t(k_buffer_size));

    rhs._state = nullptr;
    return *this;
}

cc::read_stream cc::decompressing_read_stream_adapter::stream()
{
    // Empty window, so the first read forces a refill — the buffered-adapter convention.
    return cc::read_stream(_buffer, _buffer, &impl_flush, this);
}

cc::result<i64> cc::decompressing_read_stream_adapter::impl_refill(byte*& curr, byte*& end)
{
    auto* const base = _buffer;

    // Preserve whatever the caller has not consumed, then decode into the rest.
    // The ranges overlap, so memmove.
    auto const leftover = isize(end - curr);
    CC_ASSERT(leftover < k_buffer_size, "refilling a full buffer makes no progress");
    cc::memmove(base, curr, size_t(leftover));

    auto produced = leftover;

    // Loop until at least one new byte exists or the data genuinely ends.
    // A parser driving ready_bytes()/flush() spins forever on a flush that returns nothing without being at the end,
    // so producing zero bytes here is only ever allowed when curr == end is the truth.
    while (produced == leftover && !_frame_finished)
    {
        auto available = _inner.ready_bytes();
        if (available.empty())
        {
            CC_RETURN_IF_ERROR(_inner.flush());
            available = _inner.ready_bytes();
            if (available.empty()) // the compressed source ended mid-frame
                break;
        }

        // Only a size the frame declares in its HEADER can be read here, `available` being one window of the inner
        // stream rather than the whole blob.
        // gzip declares its size in the trailer, so deflate answers no and a gzip stream simply has no hint - reading
        // the last four bytes of a partial window would return compressed payload as a length.
        auto const& backend = impl::backend_for(_algorithm);
        if (!_hint_probed && backend.declares_size_in_header)
        {
            _hint_probed = true;
            auto const declared = backend.declared_size(available);
            if (declared.has_value())
            {
                _remaining_hint = i64(declared.value());

                // The hint is attacker-controlled on untrusted input and read_all() turns it into one reservation, so
                // a cap the caller set has to bind it too.
                if (_config.max_output_size >= 0)
                    _remaining_hint = cc::min(_remaining_hint, i64(_config.max_output_size));
            }
        }

        // max_output_size is enforced by narrowing what the codec may fill: only this side knows what earlier calls
        // already produced.
        auto room = k_buffer_size - produced;
        if (_config.max_output_size >= 0)
            room = cc::min(room, isize(i64(_config.max_output_size) - _total_produced));

        auto step = backend.stream_decompress(_state, available, cc::span<byte>(base + produced, room));
        CC_RETURN_IF_ERROR(step);

        _inner.consume(step.value().consumed);
        produced += step.value().produced;
        _total_produced += i64(step.value().produced);

        if (step.value().finished)
            _frame_finished = true;

        // Neither side moved and the frame is not done: the codec wants more input than the inner stream has left.
        if (step.value().consumed == 0 && step.value().produced == 0 && !_frame_finished)
            break;
    }

    // Nothing left to give and the frame is not done, because the cap ran out rather than the data.
    // Reporting end-of-data here would hand back a silently truncated payload, which is the one outcome a cap must
    // never produce — a caller that set one is entitled to hear that it bound.
    // Bytes decoded before the cap was reached are still delivered; this fires on the pass that follows them.
    if (produced == leftover && !_frame_finished && _config.max_output_size >= 0
        && _total_produced >= _config.max_output_size)
        return cc::error(cc::format("decompressing stream: output exceeds the {} byte limit", _config.max_output_size));

    curr = base;
    end = base + produced;

    if (_remaining_hint >= 0)
        _remaining_hint -= i64(produced - leftover);

    return i64(-1); // decompressed position is not tracked, and this stream is not seekable anyway
}

cc::result<i64> cc::decompressing_read_stream_adapter::impl_flush(byte*& curr,
                                                                  byte*& end,
                                                                  byte*& /*write_end*/,
                                                                  void* ctx,
                                                                  i64 offset,
                                                                  seek_dir dir,
                                                                  byte* /*first_write*/)
{
    auto& self = *static_cast<decompressing_read_stream_adapter*>(ctx);

    switch (dir)
    {
    case sd::relative:
        if (offset == 0)
            return self.impl_refill(curr, end);
        return i64(-1);

    case sd::remaining_size_hint:
        return self._remaining_hint;

    case sd::begin:
    case sd::end:
    case sd::dry_begin:
    case sd::dry_relative:
    case sd::dry_end:
        // Reaching an earlier offset would mean decoding the frame again from the start, which is the definition of not seekable.
        // Returning -1 from dry_relative is what keeps try_as_seekable from upgrading this.
        return i64(-1);
    }

    CC_UNREACHABLE("invalid seek_dir");
}

// --- compressing write ------------------------------------------------------------------------------------

cc::result<cc::compressing_write_stream_adapter> cc::compressing_write_stream_adapter::create(cc::write_stream&& inner,
                                                                                              compression_config cfg)
{
    CC_RETURN_IF_ERROR(impl::validate_compression_config(cfg));

    if (!is_streamable(cfg.framing))
        return cc::error("compressing stream: raw framing has no wrapper to stream into, so it cannot be written this "
                         "way");

    auto adapter = compressing_write_stream_adapter();
    adapter._inner = cc::move(inner);
    adapter._config = cfg;
    adapter._state = impl::backend_for(cfg.algorithm).create_stream_compressor(cfg);

    if (adapter._state == nullptr)
        return cc::error("compressing stream: the backend refused to create a compression context");

    auto const bound = impl::backend_for(cfg.algorithm).stream_compress_bound(adapter._state, k_buffer_size);
    adapter._staging = cc::vector<byte>::create_uninitialized(bound);

    return adapter;
}

cc::compressing_write_stream_adapter::~compressing_write_stream_adapter()
{
    // Not a leak check: an unsealed frame has no terminator and no checksum, so every byte written through this adapter
    // is unreadable.
    // Losing that silently is the one outcome worth aborting a debug build over.
    CC_ASSERT(_finished || _state == nullptr, "finish() the compressing stream before dropping it, or the frame is "
                                              "unreadable");

    if (_state != nullptr)
        impl::backend_for(_config.algorithm).destroy_stream_compressor(_state);
}

// As above: clearing the source's _state is what keeps the destructor's finish() assert from firing on the husk that
// create() left behind, and what stops the context being destroyed twice.
cc::compressing_write_stream_adapter::compressing_write_stream_adapter(compressing_write_stream_adapter&& rhs) noexcept
  : _inner(cc::move(rhs._inner)),
    _config(rhs._config),
    _state(rhs._state),
    _staging(cc::move(rhs._staging)),
    _written(rhs._written),
    _finished(rhs._finished)
{
    cc::memcpy(_buffer, rhs._buffer, size_t(k_buffer_size));
    rhs._state = nullptr;
}

cc::compressing_write_stream_adapter& cc::compressing_write_stream_adapter::operator=(
    compressing_write_stream_adapter&& rhs) noexcept
{
    if (this == &rhs)
        return *this;

    CC_ASSERT(_finished || _state == nullptr, "finish() the compressing stream before overwriting it");

    if (_state != nullptr)
        impl::backend_for(_config.algorithm).destroy_stream_compressor(_state);

    _inner = cc::move(rhs._inner);
    _config = rhs._config;
    _state = rhs._state;
    _staging = cc::move(rhs._staging);
    _written = rhs._written;
    _finished = rhs._finished;
    cc::memcpy(_buffer, rhs._buffer, size_t(k_buffer_size));

    rhs._state = nullptr;
    return *this;
}

cc::write_stream cc::compressing_write_stream_adapter::stream()
{
    // Full window: for a write stream `end` is the capacity, not a data boundary.
    return cc::write_stream(_buffer, _buffer + k_buffer_size, &impl_flush, this);
}

cc::result<cc::unit> cc::compressing_write_stream_adapter::impl_emit(cc::span<byte const> in, bool finish)
{
    auto const& backend = impl::backend_for(_config.algorithm);

    auto written = backend.stream_compress(_state, in, _staging, finish);
    CC_RETURN_IF_ERROR(written);

    if (written.value() > 0)
    {
        CC_RETURN_IF_ERROR(_inner.write(cc::span<byte const>(_staging.data(), written.value())));
        _written += written.value();
    }

    return cc::unit{};
}

cc::result<i64> cc::compressing_write_stream_adapter::finish()
{
    if (_finished)
        return _written;

    CC_RETURN_IF_ERROR(impl_emit({}, /*finish*/ true));
    _finished = true;
    return _written;
}

cc::result<i64> cc::compressing_write_stream_adapter::impl_flush(byte*& curr,
                                                                 byte*& end,
                                                                 byte*& /*write_end*/,
                                                                 void* ctx,
                                                                 i64 offset,
                                                                 seek_dir dir,
                                                                 byte* first_write)
{
    auto& self = *static_cast<compressing_write_stream_adapter*>(ctx);

    switch (dir)
    {
    case sd::relative:
    {
        if (offset != 0)
            return i64(-1);

        CC_ASSERT(!self._finished, "writing to a compressing stream after finish()");

        if (first_write != nullptr && curr > first_write)
            CC_RETURN_IF_ERROR(self.impl_emit(cc::span<byte const>(first_write, curr), /*finish*/ false));

        // The whole buffer is free again.
        // Failing to hand it all back would make write() report a full sink.
        curr = self._buffer;
        end = self._buffer + k_buffer_size;
        return i64(-1);
    }

    case sd::begin:
    case sd::end:
    case sd::dry_begin:
    case sd::dry_relative:
    case sd::dry_end:
    case sd::remaining_size_hint:
        // A frame is written forwards only; nothing already handed to the codec can be revisited.
        return i64(-1);
    }

    CC_UNREACHABLE("invalid seek_dir");
}
