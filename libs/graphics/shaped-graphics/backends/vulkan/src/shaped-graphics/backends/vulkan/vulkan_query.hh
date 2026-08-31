#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/fwd.hh>

/// One VkQueryPool leased exclusively by a single command list while recording, returned to the pool after
/// submit/drop.
/// Slots are bump-allocated via next_slot.
/// Every gpu_timestamp pointing into this pool shares `shared_future`, which is assigned in place at submit.
struct sg::backend::vulkan::vulkan_query_pool_lease
{
    VkQueryPool pool = VK_NULL_HANDLE;
    int slot_count = 0;
    int next_slot = 0;

    /// Shared by every handle pointing into this pool.
    /// Default-constructed (invalid) until submit, then assigned in place with the pool's actual readback.
    /// A dropped list leaves it invalid forever.
    std::shared_ptr<sg::data_future<u64>> shared_future = std::make_shared<sg::data_future<u64>>();
};

/// Backend GPU-query system: a free-list pool of small VkQueryPools.
///
/// The same shape as dx12's, and for the same reasons — a list leases one on demand, bump-allocates slots, and at
/// submit resolves each pool into a transient buffer and starts one inline readback per pool.
///
/// **The one Vulkan-specific decision is where a pool gets reset.**
/// A query pool must be reset before its slots are written, and `vkCmdResetQueryPool` may not be recorded inside a
/// render-pass instance — but a timestamp legitimately can be, so a reset recorded at lease time would land there.
/// So this resets on the HOST instead, when a pool is handed out, which the hostQueryReset feature makes possible and
/// which has no such restriction.
/// D3D12 needs none of this: a query heap has no reset step at all.
///
/// All public methods are threadsafe: multiple command lists may lease and release pools concurrently.
class sg::backend::vulkan::vulkan_query_system
{
public:
    /// Slots per pool, intentionally small: it bounds each readback chunk, and a list recording more simply leases
    /// additional pools.
    static constexpr int SlotsPerPool = 4096;

    /// Reads the device's timestamp period and whether the graphics queue family times at all.
    /// Called once during context bring-up, after the queue family is known.
    void initialize(vulkan_context& ctx);

    /// Whether GPU timestamps are supported: the queue family must report a non-zero timestampValidBits, and the
    /// device must offer hostQueryReset, which is how a pool is reset here.
    [[nodiscard]] bool supports_timestamps() const { return _supports_timestamps; }

    /// Multiplier from raw ticks to seconds — timestampPeriod, which Vulkan reports in nanoseconds.
    [[nodiscard]] double timestamp_tick_to_seconds() const { return _timestamp_tick_to_seconds; }

    /// Leases a pool with all slots free and already reset.
    [[nodiscard]] cc::unique_ptr<vulkan_query_pool_lease> acquire_pool();

    /// Returns a pool: resets next_slot and installs a fresh, invalid shared_future for the next leaseholder.
    /// Handles from the previous lease keep their own, now-real, future.
    void release_pool(cc::unique_ptr<vulkan_query_pool_lease> lease);

    /// Destroys all pooled query pools, before the device goes.
    void shutdown();

private:
    vulkan_context* _ctx = nullptr;
    bool _supports_timestamps = false;
    double _timestamp_tick_to_seconds = 0.0;

    /// Pools here have next_slot == 0 and a fresh invalid shared_future.
    cc::mutex<cc::vector<cc::unique_ptr<vulkan_query_pool_lease>>> _free_list;
};
