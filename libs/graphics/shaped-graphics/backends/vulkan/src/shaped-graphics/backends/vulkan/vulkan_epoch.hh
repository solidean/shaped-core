#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>

// Per-epoch bookkeeping for the vulkan backend's epoch system.
// The epoch *concept* lives in sg:: — fwd.hh plus the sg::context contract — and this is vulkan's concrete realization on a pair of timeline semaphores.
// See libs/graphics/shaped-graphics/docs/concepts/epochs.md.

namespace sg::backend::vulkan
{
/// A command pool and the single command buffer allocated from it.
/// Recycled as a unit: resetting the pool recycles its buffer.
/// Idle pools live in the pool set; in-flight ones ride along in the owning epoch until it retires.
struct vulkan_command_pool
{
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE; // owned by the pool; reset with it, not freed separately
};

/// A GPU resource captured for deferred deletion.
/// Its handles and backing memory are freed, and its finalizers run, only once the owning epoch has retired — i.e. the GPU is no longer using it.
struct vulkan_expiring_resource
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    cc::vector<cc::unique_function<void()>> finalizers;
};

/// Everything one epoch owns and must reclaim once its GPU work finishes.
/// Built at advance, drained at retire.
struct vulkan_epoch_data
{
    epoch epoch_id = epoch::invalid;
    cc::vector<vulkan_command_pool> command_pools; // reset + returned to the pool set on retire
    cc::vector<vulkan_expiring_resource> expiring; // freed on retire
};

/// The command-pool set: pools captured from lists submitted in the current epoch, moved into the epoch payload at advance, plus idle pools ready for reuse.
/// A never-submitted list returns its pool straight to `free`, since the GPU never saw it.
/// Guarded by a mutex because create / submit / drop are thread-safe.
///
/// TODO: promote to a standalone object owning its own synchronization, with pools per queue once the epoch system grows multiple queues.
struct vulkan_command_pool_set
{
    cc::vector<vulkan_command_pool> in_epoch; // captured by submitted lists this epoch
    cc::vector<vulkan_command_pool> free;     // idle, ready for reuse (reset on reuse)
};

/// The mutex-guarded epoch state: the in-flight FIFO plus the deferred-deletion staging area.
/// Guarded because a resource's refcount can hit zero — staging a deletion — on any thread, while advance and retire run externally synchronized.
struct vulkan_epoch_state
{
    cc::vector<vulkan_epoch_data> in_flight;     // FIFO, oldest at the front
    cc::vector<vulkan_expiring_resource> staged; // refcount-zero resources awaiting the next advance
};

/// Reclaims one expiring resource in the required order.
/// Free the GPU handles *first*, releasing GPU memory, then move its finalizers into `out_finalizers` to run once the caller has left any lock.
/// A finalizer must never observe a resource whose handles are still live.
inline void release_expiring(VkDevice device,
                             vulkan_expiring_resource& r,
                             cc::vector<cc::unique_function<void()>>& out_finalizers)
{
    if (r.buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, r.buffer, nullptr);
    if (r.image != VK_NULL_HANDLE)
        vkDestroyImage(device, r.image, nullptr);
    if (r.memory != VK_NULL_HANDLE)
        vkFreeMemory(device, r.memory, nullptr);
    r.buffer = VK_NULL_HANDLE;
    r.image = VK_NULL_HANDLE;
    r.memory = VK_NULL_HANDLE;
    for (auto& f : r.finalizers)
        out_finalizers.push_back(cc::move(f));
    r.finalizers.clear();
}
} // namespace sg::backend::vulkan
