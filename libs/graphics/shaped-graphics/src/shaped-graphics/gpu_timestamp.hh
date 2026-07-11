#pragma once

#include <clean-core/error/optional.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

#include <memory>

namespace sg
{
/// Result of a `cmd.query.record_gpu_timestamp()` — a pending read of one GPU timestamp. A small,
/// copyable value type (the query analogue of bytes_future).
///
/// Lifecycle: recorded into a command list, its raw tick lands on the host once that list has been
/// submitted AND its readback has finished on the GPU. Read the result only after submitting the
/// recording list (typically after a ctx.wait_for_ticks/seconds or a successful is_ready() poll);
/// reading before submit reports not-ready. Values are only meaningful as differences — two timestamps
/// around some work give that work's GPU duration.
///
/// is_valid() reflects whether the backend supports timestamps (a default-constructed timestamp, and
/// every timestamp from a backend without timestamp support, is invalid). It says nothing about whether
/// the read has completed — that is is_ready().
class gpu_timestamp
{
    // ctx.wait_for_ticks/seconds block on the shared future and read this timestamp — kept off the public API.
    friend class context;

public:
    /// An invalid timestamp — not backed by any recorded query (also what an unsupported backend returns).
    gpu_timestamp() = default;

    /// Backend seam: binds a timestamp to the shared per-heap future its tick is downloaded into, the
    /// element `index` within that future, and the `tick_to_seconds` factor (1 / timestamp frequency).
    /// `heap_future` is shared by every timestamp of the same heap and filled in place at submit. This
    /// is the single public seam a backend uses to construct a timestamp.
    gpu_timestamp(std::shared_ptr<data_future<u64> const> heap_future, isize index, double tick_to_seconds)
      : _heap_future(cc::move(heap_future)), _index(index), _tick_to_seconds(tick_to_seconds)
    {
    }

    /// Whether this timestamp is backed by a real recorded query (vs default-constructed / unsupported).
    [[nodiscard]] bool is_valid() const { return _heap_future != nullptr; }

    /// Non-blocking poll: whether the tick has been read back to the host. False before the recording
    /// list is submitted, and forever if that list was dropped.
    [[nodiscard]] bool is_ready() const;

    /// The raw GPU tick if ready (polls), else nullopt. Only differences are meaningful.
    [[nodiscard]] cc::optional<u64> try_get_ticks() const;

    /// The tick converted to seconds since an unspecified epoch if ready (polls), else nullopt. Only
    /// differences are meaningful.
    [[nodiscard]] cc::optional<double> try_get_seconds() const;

private:
    /// Shared future for the whole heap (one download per heap); every timestamp of that heap aliases
    /// it and indexes its own slot. Null iff invalid. Default-invalid until the recording list submits,
    /// then assigned in place by the backend.
    std::shared_ptr<data_future<u64> const> _heap_future;

    /// This timestamp's element index within `_heap_future`'s downloaded array.
    isize _index = 0;

    /// Multiplier from raw GPU ticks to seconds (1 / timestamp frequency).
    double _tick_to_seconds = 0.0;
};
} // namespace sg
