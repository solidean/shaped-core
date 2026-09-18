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

/// One MTL4 counter heap, leased exclusively by a single command list while it records and returned after submit or
/// drop.
/// Slots are bump-allocated through `next_slot`.
/// Every `gpu_timestamp` pointing into this heap shares `shared_future`, which is assigned in place at submit.
struct sg::backend::metal::metal_counter_heap_lease
{
    MTL4::CounterHeap* heap = nullptr;
    int slot_count = 0;
    int next_slot = 0;

    /// Shared by every handle pointing into this heap.
    /// Default-constructed (invalid) until submit, then assigned in place with the heap's actual readback.
    /// A dropped list leaves it invalid forever, which is what "never ready" means for a timestamp whose list never ran.
    std::shared_ptr<sg::data_future<u64>> shared_future = std::make_shared<sg::data_future<u64>>();
};

/// Backend GPU-query system: a free list of MTL4 counter heaps.
///
/// The same shape as vulkan's and dx12's — a list leases one on demand, bump-allocates slots, and at submit resolves
/// each heap and starts one inline readback per heap.
///
/// **Metal needs neither of the other two backends' awkward steps.**
/// A timestamp is written by `MTL4::CommandBuffer::writeTimestampIntoHeap`, at command-buffer level rather than inside
/// an encoder — so there is no render-pass restriction to work around, and no host reset either: a heap's entries are
/// invalidated on release, whenever that happens to be.
/// The resolve writes straight into the download ring, so unlike vulkan there is no transient buffer in between.
///
/// All public methods are threadsafe: several command lists may lease and release heaps concurrently.
class sg::backend::metal::metal_query_system
{
public:
    /// Slots per heap, intentionally small: it bounds each readback chunk, and a list recording more simply leases
    /// further heaps.
    static constexpr int SlotsPerHeap = 4096;

    /// Creates the first heap, which is also the support probe.
    /// A device that refuses one reports no timestamps rather than failing context creation.
    [[nodiscard]] cc::result<cc::unit> create(metal_context& ctx);

    [[nodiscard]] bool supports_timestamps() const { return _supports_timestamps; }

    /// Multiplier from raw ticks to seconds.
    ///
    /// Metal reports GPU and CPU timestamps on one timebase here, which `MTL::Device::sampleTimestamps` shows directly:
    /// over a measured interval the two deltas are equal, at 1e9 ticks per second.
    /// tests/metal-query-test.cc pins that, so a device that ever stopped agreeing would be caught rather than silently
    /// scaling every measurement.
    [[nodiscard]] double timestamp_tick_to_seconds() const { return _tick_to_seconds; }

    /// Leases a heap with every slot free.
    /// Null when timestamps are unsupported, or when the device refuses a new heap.
    [[nodiscard]] cc::unique_ptr<metal_counter_heap_lease> acquire_heap();

    /// Returns a heap: invalidates the slots it used, resets `next_slot`, and installs a fresh, invalid shared future
    /// for the next leaseholder.
    /// Handles from the previous lease keep their own, now-real, future.
    void release_heap(cc::unique_ptr<metal_counter_heap_lease> lease);

    /// Releases every pooled heap, before the device goes.
    void shutdown();

private:
    [[nodiscard]] cc::unique_ptr<metal_counter_heap_lease> create_heap();

    metal_context* _ctx = nullptr;
    bool _supports_timestamps = false;
    double _tick_to_seconds = 0.0;

    /// Heaps here have `next_slot == 0` and a fresh invalid shared future.
    cc::mutex<cc::vector<cc::unique_ptr<metal_counter_heap_lease>>> _free_list;
};
