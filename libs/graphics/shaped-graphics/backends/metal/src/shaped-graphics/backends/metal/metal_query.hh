#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh> // sg's door to <memory>, for the shared per-heap future

namespace MTL4
{
class CounterHeap;
}

/// A range of one MTL4 counter heap's slots, leased exclusively by a single command list while it records and returned
/// after submit or drop.
/// Slots are bump-allocated through `next_slot`, counted from `first_slot`.
/// Every `gpu_timestamp` pointing into this range shares `shared_future`, which is assigned in place at submit.
struct sg::backend::metal::metal_counter_heap_lease
{
    MTL4::CounterHeap* heap = nullptr;
    int first_slot = 0;
    int slot_count = 0;
    int next_slot = 0;

    /// Shared by every handle pointing into this range.
    /// Default-constructed (invalid) until submit, then assigned in place with the range's actual readback.
    /// A dropped list leaves it invalid forever, which is what "never ready" means for a timestamp whose list never ran.
    std::shared_ptr<sg::data_future<u64>> shared_future = std::make_shared<sg::data_future<u64>>();
};

/// Backend GPU-query system: the context's MTL4 counter heaps, handed out as ranges of slots.
///
/// The same shape as vulkan's and dx12's — a list leases a range on demand, bump-allocates slots, and at submit
/// resolves each range and starts one inline readback per range.
///
/// **A list leases a range, never a whole heap**, because metal grants a process 32 counter heaps whatever their size.
/// A heap a list's two timestamps held for itself ran out once 32 lists were in flight.
///
/// **Metal needs neither of the other two backends' awkward steps.**
/// A timestamp is written by `MTL4::CommandBuffer::writeTimestampIntoHeap`, at command-buffer level rather than inside
/// an encoder — so there is no render-pass restriction to work around, and no host reset either: a range's entries are
/// invalidated on release, whenever that happens to be.
/// The resolve writes straight into the download ring, so unlike vulkan there is no transient buffer in between.
///
/// All public methods are threadsafe: several command lists may lease and release ranges concurrently.
class sg::backend::metal::metal_query_system
{
public:
    /// Slots per heap; a larger heap fails metal's validation.
    static constexpr int SlotsPerHeap = 4096;

    /// Slots per leased range, intentionally small: a heap serves `SlotsPerHeap / SlotsPerLease` lists at once, and a
    /// list recording more simply leases further ranges.
    static constexpr int SlotsPerLease = 64;

    /// Makes no heap: the first is made when the first timestamp is recorded.
    [[nodiscard]] cc::result<cc::unit> create(metal_context& ctx);

    [[nodiscard]] bool supports_timestamps() const { return _supports_timestamps; }

    /// Multiplier from raw ticks to seconds.
    ///
    /// Metal reports GPU and CPU timestamps on one timebase here, which `MTL::Device::sampleTimestamps` shows directly:
    /// over a measured interval the two deltas are equal, at 1e9 ticks per second.
    /// tests/metal-query-test.cc pins that, so a device that ever stopped agreeing would be caught rather than silently
    /// scaling every measurement.
    [[nodiscard]] double timestamp_tick_to_seconds() const { return _tick_to_seconds; }

    /// Leases a range of `SlotsPerLease` free slots, making a heap when no range is free.
    /// Null when timestamps are unsupported, or when the device refuses a new heap, which is reported as a device error.
    [[nodiscard]] cc::unique_ptr<metal_counter_heap_lease> acquire_lease();

    /// Returns a range: invalidates the slots it used, resets `next_slot`, and installs a fresh, invalid shared future
    /// for the next leaseholder.
    /// Handles from the previous lease keep their own, now-real, future.
    void release_lease(cc::unique_ptr<metal_counter_heap_lease> lease);

    /// Releases every heap, before the device goes.
    /// Runs after the epoch drain, which returns every range a submitted list resolved.
    void shutdown();

private:
    struct pool
    {
        /// Every heap made, owned here whether its ranges are leased or free.
        cc::vector<MTL4::CounterHeap*> heaps;

        /// Ranges here have `next_slot == 0` and a fresh invalid shared future.
        cc::vector<cc::unique_ptr<metal_counter_heap_lease>> free;
    };

    metal_context* _ctx = nullptr;
    bool _supports_timestamps = false;
    double _tick_to_seconds = 0.0;

    cc::mutex<pool> _pool;
};
