#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/vulkan/vulkan_completion_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>

namespace sg::backend::vulkan
{
bool vulkan_completion_group::has_reached(u64 value) const
{
    if (value == 0 || timeline == VK_NULL_HANDLE || ctx == nullptr)
        return true;

    u64 current = 0;
    vkGetSemaphoreCounterValue(ctx->_device, timeline, &current);
    return current >= value;
}

void vulkan_completion_group_pool::initialize(vulkan_context& ctx)
{
    _ctx = &ctx;
    _free = std::make_shared<free_list>();
}

vulkan_completion_group_handle vulkan_completion_group_pool::acquire()
{
    CC_ASSERT(_ctx != nullptr, "the completion-group pool was not initialized");

    // The deleter returns the group to the pool, or destroys it when the pool is already gone.
    auto free = _free;
    auto const deleter = [free](vulkan_completion_group* g)
    {
        if (g == nullptr)
            return;
        // The liveness check belongs INSIDE the lock, since shutdown drains the list under it: read outside, a
        // shutdown landing between the read and the push adds `g` to a vector nothing will drain again, and its
        // semaphore is then alive at vkDestroyDevice.
        bool returned = false;
        free->groups.lock(
            [&](cc::vector<vulkan_completion_group*>& groups)
            {
                if (!free->alive.load(cc::memory_order_acquire))
                    return;
                groups.push_back(g);
                returned = true;
            });
        if (returned)
            return;

        // The pool is gone, so this is teardown: destroy the semaphore rather than leaking it.
        if (g->timeline != VK_NULL_HANDLE && g->ctx != nullptr)
            vkDestroySemaphore(g->ctx->_device, g->timeline, nullptr);
        delete g;
    };

    vulkan_completion_group* recycled = nullptr;
    _free->groups.lock(
        [&](cc::vector<vulkan_completion_group*>& groups)
        {
            if (!groups.empty())
                recycled = groups.pop_back();
        });
    if (recycled != nullptr)
        return vulkan_completion_group_handle(recycled, deleter);

    auto const info = VkSemaphoreTypeCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    auto const create = VkSemaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &info};

    auto* const group = new vulkan_completion_group();
    group->ctx = _ctx;
    VkResult const r = vkCreateSemaphore(_ctx->_device, &create, nullptr, &group->timeline);
    CC_ASSERT(r == VK_SUCCESS, "vkCreateSemaphore (completion group) failed");
    return vulkan_completion_group_handle(group, deleter);
}

void vulkan_completion_group_pool::shutdown()
{
    if (_free == nullptr)
        return;

    // Marked dead first, so a group released after this destroys its own semaphore rather than joining a list
    // nothing will drain.
    _free->alive.store(false, cc::memory_order_release);
    _free->groups.lock(
        [&](cc::vector<vulkan_completion_group*>& groups)
        {
            for (auto* const g : groups)
            {
                if (g->timeline != VK_NULL_HANDLE)
                    vkDestroySemaphore(_ctx->_device, g->timeline, nullptr);
                delete g;
            }
            groups.clear();
        });
}
} // namespace sg::backend::vulkan
