// vulkan_command_list: allocation, submission, drop, and teardown. The list type is header-only
// (ctor + fields); its create/submit/drop bodies and destructor live here.

#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

namespace sg::backend::vulkan
{
vulkan_command_list::~vulkan_command_list()
{
    // Safety net for a list neither submitted nor dropped: destroying the pool frees its command
    // buffer. Submit/drop null _pool after handing it back, so in the normal path this does nothing.
    if (_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(_ctx._device, _pool, nullptr);
}

cc::result<std::unique_ptr<vulkan_command_list>> vulkan_context::create_vulkan_command_list()
{
    // Reuse a pooled command pool if one is free (it re-entered the pool only once idle), else make
    // one. The pool + its single buffer are recycled as a unit, epoch-gated like dx12's allocator.
    vulkan_command_pool const reused = _command_pools.lock(
        [](vulkan_command_pool_set& p) -> vulkan_command_pool
        {
            if (p.free.empty())
                return {};
            return p.free.pop_back();
        });

    VkCommandPool pool = reused.pool;
    VkCommandBuffer buffer = reused.buffer;
    if (pool != VK_NULL_HANDLE)
    {
        // Recycle: reset returns the pool's buffer to the initial state, ready to record into again.
        if (VkResult r = vkResetCommandPool(_device, pool, 0); r != VK_SUCCESS)
            return vulkan_error(r, "vkResetCommandPool failed");
    }
    else
    {
        // A pool per list mirrors dx12's per-list allocator: simple ownership, recycled by epoch.
        auto const pool_info = VkCommandPoolCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = _queue_family_index,
        };
        if (VkResult r = vkCreateCommandPool(_device, &pool_info, nullptr, &pool); r != VK_SUCCESS)
            return vulkan_error(r, "vkCreateCommandPool failed");

        auto const alloc_info = VkCommandBufferAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (VkResult r = vkAllocateCommandBuffers(_device, &alloc_info, &buffer); r != VK_SUCCESS)
        {
            vkDestroyCommandPool(_device, pool, nullptr);
            return vulkan_error(r, "vkAllocateCommandBuffers failed");
        }
    }

    // Handed out already recording; submit ends it.
    auto const begin_info = VkCommandBufferBeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (VkResult r = vkBeginCommandBuffer(buffer, &begin_info); r != VK_SUCCESS)
    {
        vkDestroyCommandPool(_device, pool, nullptr);
        return vulkan_error(r, "vkBeginCommandBuffer failed");
    }

    _open_command_lists.fetch_add(1, std::memory_order_relaxed); // must reach 0 before the epoch can advance
    // Stamped with the epoch it must be submitted/dropped in.
    return std::make_unique<vulkan_command_list>(*this, current_epoch(), pool, buffer);
}

sg::submission_token vulkan_context::submit_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot submit a null command list");
    CC_ASSERT(cmd->created_in_epoch() == current_epoch(), "a command list must be submitted in the epoch it was opened "
                                                          "in (it cannot span epochs)");

    VkResult const end = vkEndCommandBuffer(cmd->_buffer);
    CC_ASSERT(end == VK_SUCCESS, "vkEndCommandBuffer failed");

    // Take a monotonic completion token and submit — all under one lock so token order equals queue
    // submission + signal order. (The queue is free-threaded, but out-of-order signals would move the
    // submission timeline backwards and break is_submission_complete.)
    sg::submission_token const token = _next_submission.lock(
        [&](sg::submission_token& next)
        {
            sg::submission_token const t = next;
            next = sg::submission_token(cc::u64(next) + 1);

            cc::u64 const signal_value = cc::u64(t);
            auto const timeline_info = VkTimelineSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .signalSemaphoreValueCount = 1,
                .pSignalSemaphoreValues = &signal_value,
            };
            auto const submit = VkSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = &timeline_info,
                .commandBufferCount = 1,
                .pCommandBuffers = &cmd->_buffer,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &_submission_timeline,
            };
            VkResult const sr = vkQueueSubmit(_queue, 1, &submit, VK_NULL_HANDLE);
            CC_ASSERT(sr == VK_SUCCESS, "vkQueueSubmit failed");
            return t;
        });

    // The pool is in flight until this epoch retires — hand it to the current epoch. Null the list's
    // handles so its dtor doesn't destroy the pool we just handed off.
    _command_pools.lock([&](vulkan_command_pool_set& p) { p.in_epoch.push_back({cmd->_pool, cmd->_buffer}); });
    cmd->_pool = VK_NULL_HANDLE;
    cmd->_buffer = VK_NULL_HANDLE;
    _open_command_lists.fetch_sub(1, std::memory_order_relaxed);
    return token;
}

void vulkan_context::drop_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot drop a null command list");
    CC_ASSERT(cmd->created_in_epoch() == current_epoch(), "a command list must be dropped in the epoch it was opened "
                                                          "in");

    // Never submitted, so the GPU never touched this pool — return it straight to the free set (reset
    // happens at reuse). Null the list's handles so its dtor doesn't destroy the pool.
    _command_pools.lock([&](vulkan_command_pool_set& p) { p.free.push_back({cmd->_pool, cmd->_buffer}); });
    cmd->_pool = VK_NULL_HANDLE;
    cmd->_buffer = VK_NULL_HANDLE;
    _open_command_lists.fetch_sub(1, std::memory_order_relaxed);
}
} // namespace sg::backend::vulkan
