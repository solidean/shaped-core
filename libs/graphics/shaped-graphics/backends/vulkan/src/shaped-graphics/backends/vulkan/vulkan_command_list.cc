// vulkan_command_list: allocation, submission, drop, and teardown. The list type is header-only
// (ctor + fields); its create/submit/drop bodies and destructor live here.

#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

namespace sg::backend::vulkan
{
vulkan_command_list::~vulkan_command_list()
{
    // Destroying the pool frees the command buffer allocated from it.
    if (_pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(_ctx._device, _pool, nullptr);
}

cc::result<std::unique_ptr<vulkan_command_list>> vulkan_context::create_vulkan_command_list()
{
    // A pool per list mirrors dx12's per-list allocator: simple ownership, freed with the list. A
    // shared/reset pool is a later optimization.
    auto const pool_info = VkCommandPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = _queue_family_index,
    };

    VkCommandPool pool = VK_NULL_HANDLE;
    if (VkResult r = vkCreateCommandPool(_device, &pool_info, nullptr, &pool); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateCommandPool failed");

    auto const alloc_info = VkCommandBufferAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };

    VkCommandBuffer buffer = VK_NULL_HANDLE;
    if (VkResult r = vkAllocateCommandBuffers(_device, &alloc_info, &buffer); r != VK_SUCCESS)
    {
        vkDestroyCommandPool(_device, pool, nullptr);
        return vulkan_error(r, "vkAllocateCommandBuffers failed");
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

    return std::make_unique<vulkan_command_list>(*this, pool, buffer);
}

void vulkan_context::submit_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot submit a null command list");

    VkResult r = vkEndCommandBuffer(cmd->_buffer);
    CC_ASSERT(r == VK_SUCCESS, "vkEndCommandBuffer failed");

    auto const submit = VkSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd->_buffer,
    };
    r = vkQueueSubmit(_queue, 1, &submit, VK_NULL_HANDLE);
    CC_ASSERT(r == VK_SUCCESS, "vkQueueSubmit failed");

    // No fence yet: wait for the queue to drain so the pool (destroyed with cmd right after this
    // returns) isn't freed while its buffer is still pending. Cheap while today's lists carry no real
    // work; real fencing + device-loss handling land with the sync milestone. dx12's submit takes the
    // same no-fence shortcut.
    vkQueueWaitIdle(_queue);

    // cmd is destroyed here (leaves scope) — its dtor destroys the pool.
}

void vulkan_context::drop_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot drop a null command list");
    // cmd is destroyed here — the explicit form of letting it leave scope. Its dtor destroys the pool.
}
} // namespace sg::backend::vulkan
