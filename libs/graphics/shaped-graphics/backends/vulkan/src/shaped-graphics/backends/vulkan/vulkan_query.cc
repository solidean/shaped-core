// vulkan_query: the VkQueryPool half of cmd.query.
// See libs/graphics/shaped-graphics/docs/concepts/queries.md.

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_query.hh>

namespace sg::backend::vulkan
{
void vulkan_query_system::initialize(vulkan_context& ctx)
{
    _ctx = &ctx;

    // timestampPeriod is nanoseconds per tick, so the conversion is a constant rather than dx12's queue frequency
    // query.
    // A device property here, a queue one there.
    _timestamp_tick_to_seconds = double(ctx.device_properties().limits.timestampPeriod) * 1e-9;

    // The queue family has to actually time.
    // A family reporting zero valid bits writes nothing useful, which is a legitimate device rather than a failure —
    // the honest answer is then "unsupported".
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx._physical_device, &count, nullptr);
    auto families = cc::vector<VkQueueFamilyProperties>::create_uninitialized(count);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx._physical_device, &count, families.data());

    bool const family_times
        = ctx.queue_family_index() < count && families[isize(ctx.queue_family_index())].timestampValidBits > 0;

    _supports_timestamps = family_times && _timestamp_tick_to_seconds > 0.0 && ctx.supports_host_query_reset();
}

cc::unique_ptr<vulkan_query_pool_lease> vulkan_query_system::acquire_pool()
{
    CC_ASSERT(_ctx != nullptr, "the query system was not initialized");

    cc::unique_ptr<vulkan_query_pool_lease> lease;
    _free_list.lock(
        [&](cc::vector<cc::unique_ptr<vulkan_query_pool_lease>>& free)
        {
            if (!free.empty())
                lease = free.pop_back();
        });

    if (lease == nullptr)
    {
        auto const info = VkQueryPoolCreateInfo{
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = u32(SlotsPerPool),
        };
        VkQueryPool pool = VK_NULL_HANDLE;
        VkResult const r = vkCreateQueryPool(_ctx->_device, &info, nullptr, &pool);
        CC_ASSERT(r == VK_SUCCESS, "vkCreateQueryPool failed");

        lease = cc::make_unique<vulkan_query_pool_lease>();
        lease->pool = pool;
        lease->slot_count = SlotsPerPool;
    }

    // Reset on the host, so nothing has to be recorded — which is what keeps a timestamp legal inside a rendering
    // scope, where vkCmdResetQueryPool is not.
    vkResetQueryPool(_ctx->_device, lease->pool, 0, u32(lease->slot_count));
    lease->next_slot = 0;
    return lease;
}

void vulkan_query_system::release_pool(cc::unique_ptr<vulkan_query_pool_lease> lease)
{
    if (lease == nullptr)
        return;

    lease->next_slot = 0;
    // A fresh node for the next leaseholder: the handles from this lease keep the one they were given, which by now
    // holds their real readback.
    lease->shared_future = std::make_shared<sg::data_future<u64>>();
    _free_list.lock([&](cc::vector<cc::unique_ptr<vulkan_query_pool_lease>>& free) { free.push_back(cc::move(lease)); });
}

void vulkan_query_system::shutdown()
{
    if (_ctx == nullptr)
        return;

    _free_list.lock(
        [&](cc::vector<cc::unique_ptr<vulkan_query_pool_lease>>& free)
        {
            for (auto const& lease : free)
                if (lease->pool != VK_NULL_HANDLE)
                    vkDestroyQueryPool(_ctx->_device, lease->pool, nullptr);
            free.clear();
        });
    _ctx = nullptr;
}
} // namespace sg::backend::vulkan
