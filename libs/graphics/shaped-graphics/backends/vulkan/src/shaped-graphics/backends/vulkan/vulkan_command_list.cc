// vulkan_command_list: allocation, submission, drop, and teardown.
// The list type itself is header-only, so its create / submit / drop bodies and its destructor live here.

#include <clean-core/common/log.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/string/print.hh>
#include <shaped-graphics/backends/vulkan/vulkan_barrier.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/exceptions.hh>

namespace sg::backend::vulkan
{
vulkan_command_list::vulkan_command_list(vulkan_context& ctx,
                                         sg::epoch created_in,
                                         sg::command_list_slot slot,
                                         VkCommandPool pool,
                                         VkCommandBuffer buffer)
  : sg::command_list(ctx, created_in), _ctx(ctx), _slot(slot), _pool(pool), _buffer(buffer)
{
}

vulkan_command_list::~vulkan_command_list()
{
    if (_consumed)
        return; // submitted or dropped explicitly — nothing to reclaim

    // Safety net: a list left neither submitted nor dropped.
    // Reclaim it like a drop so the open-list count, the slot and the pool don't leak — but warn, since the explicit call is required.
    CC_LOG_WARNING("command list destroyed without submit or drop — auto-dropping. Submit or drop every "
                   "command list explicitly through the context.");
    _ctx.reclaim_unsubmitted_command_list(*this);
}

cc::result<std::unique_ptr<vulkan_command_list>> vulkan_context::create_vulkan_command_list()
{
    // Reuse a pooled command pool if one is free — a pool re-enters the free set only once idle.
    // The pool and its single buffer are recycled as a unit.
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
        // A pool per list: simple ownership, recycled by epoch.
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
    // Stamped with the epoch it must be submitted/dropped in, plus an access-tracking slot freed on submit/drop.
    return std::make_unique<vulkan_command_list>(*this, current_epoch(), _command_list_slots.acquire(), pool, buffer);
}

sg::submission_token vulkan_context::submit_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot submit a null command list");
    CC_ASSERT(cmd->created_in_epoch() == current_epoch(), "a command list must be submitted in the epoch it was opened "
                                                          "in (it cannot span epochs)");

    // Finalize before closing: what this list leaves in flight is what the next list must synchronize against.
    // Only the last list tracking a buffer promotes it.
    // Under the same lock as the submit below, so finalize order matches submit order — the later list's canonical
    // state has to be the one that actually ran last.
    for (auto const* buffer : cmd->_touched_buffers)
        buffer->finalize_slot(cmd->slot());
    cmd->_touched_buffers.clear();

    VkResult const end = vkEndCommandBuffer(cmd->_buffer);
    CC_ASSERT(end == VK_SUCCESS, "vkEndCommandBuffer failed");

    // Take a monotonic completion token and submit, all under one lock, so token order equals queue submission + signal order.
    // The queue is free-threaded, but out-of-order signals would move the submission timeline backwards and break is_submission_complete.
    sg::submission_token const token = _next_submission.lock(
        [&](sg::submission_token& next)
        {
            sg::submission_token const t = next;
            next = sg::submission_token(u64(next) + 1);

            u64 const signal_value = u64(t);
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
            // Record device loss here but don't throw inside the lock; the throw happens after it releases.
            if (sr != VK_SUCCESS && !note_device_lost_if_lost(sr, "vkQueueSubmit"))
                CC_ASSERT(false, "vkQueueSubmit failed");
            return t;
        });

    // The submit above may have observed device loss, marked rather than thrown inside the lock.
    // Surface it now that the lock is released — the context is dead, so the post-submit bookkeeping is moot.
    if (is_device_lost())
        throw sg::device_lost_exception(device_loss_reason());

    // The pool is in flight until this epoch retires, so hand it to the current epoch.
    // Null the list's handles so its destructor cannot destroy the pool just handed off.
    _command_pools.lock([&](vulkan_command_pool_set& p) { p.in_epoch.push_back({cmd->_pool, cmd->_buffer}); });
    cmd->_pool = VK_NULL_HANDLE;
    cmd->_buffer = VK_NULL_HANDLE;
    (void)_command_list_slots.release(cmd->slot());
    _open_command_lists.fetch_sub(1, std::memory_order_relaxed);
    cmd->_consumed = true; // its dtor must not auto-drop it
    return token;
}

void vulkan_context::drop_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot drop a null command list");
    CC_ASSERT(cmd->created_in_epoch() == current_epoch(), "a command list must be dropped in the epoch it was opened "
                                                          "in");
    reclaim_unsubmitted_command_list(*cmd);
}

void vulkan_context::reclaim_unsubmitted_command_list(vulkan_command_list& cmd)
{
    CC_ASSERT(!cmd._consumed, "command list already submitted or dropped");
    cmd._consumed = true;

    // The recorded work never runs, so this list's declared accesses leave no hazard behind and canonical is
    // untouched — including when it was the last list tracking a buffer.
    for (auto const* buffer : cmd._touched_buffers)
        buffer->discard_slot(cmd.slot());
    cmd._touched_buffers.clear();

    // Never submitted, so the GPU never touched this pool — return it straight to the free set, where reset happens at reuse.
    // Null the handles so nothing double-frees them.
    _command_pools.lock([&](vulkan_command_pool_set& p) { p.free.push_back({cmd._pool, cmd._buffer}); });
    cmd._pool = VK_NULL_HANDLE;
    cmd._buffer = VK_NULL_HANDLE;
    (void)_command_list_slots.release(cmd.slot());
    _open_command_lists.fetch_sub(1, std::memory_order_relaxed);
}

void vulkan_command_list::track_buffer_access(vulkan_buffer const& buffer,
                                              sg::pipeline_stage_flags stages,
                                              sg::access_flags access)
{
    // An empty buffer owns no VkBuffer, so there is nothing to synchronize on.
    if (buffer._buffer == VK_NULL_HANDLE)
        return;

    buffer.declare_access(_slot, stages, access);

    // Once per op, however many times this buffer is bound to it.
    if (buffer.mark_pending_barrier(_slot))
        _pending_barrier_buffers.push_back(&buffer);

    // Once per list, so submit can finalize the slot and drop can discard it.
    if (buffer.mark_recorded(_slot))
        _touched_buffers.push_back(&buffer);
}

void vulkan_command_list::flush_barriers()
{
    for (auto const* buffer : _pending_barrier_buffers)
        if (auto const barrier = buffer->flush_access(_slot); barrier.needed)
            _pending_buffer_barriers.push_back(make_buffer_barrier(buffer->_buffer, barrier));

    _pending_barrier_buffers.clear();
    submit_barriers(_buffer, _pending_buffer_barriers, {});
    _pending_buffer_barriers.clear();
}

void vulkan_command_list::upload_bytes_to_buffer(sg::raw_buffer_handle buffer,
                                                 cc::span<byte const> data,
                                                 isize offset_in_bytes)
{
    // sg has already bounds-checked the write and rejected a null destination; an empty one is a no-op by contract.
    if (data.empty())
        return;

    auto const& dst = static_cast<vulkan_buffer const&>(*buffer);
    CC_ASSERT(dst._buffer != VK_NULL_HANDLE, "cannot upload into an empty buffer");

    // Stage the bytes first: reserving can block on an in-flight epoch, and doing that before anything is declared
    // keeps the tracking state consistent whichever way the wait goes.
    auto const staging = _ctx._upload_inline.reserve(data.size());
    cc::memcpy(staging.mapped, data.data(), size_t(data.size()));

    track_buffer_access(dst, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    flush_barriers();

    auto const region = VkBufferCopy{
        .srcOffset = VkDeviceSize(staging.offset),
        .dstOffset = VkDeviceSize(offset_in_bytes),
        .size = VkDeviceSize(data.size()),
    };
    vkCmdCopyBuffer(_buffer, staging.buffer, dst._buffer, 1, &region);
}

// False until the recording paths below exist, whatever the device offers.
// The question is whether THIS command list can trace rays, not whether the GPU could: answering yes while every
// build and dispatch seam is a stub turns a clean skip into a failure and tells a caller nothing it can act on.
// vulkan_context::is_raytracing_supported() holds the device's answer, ready for this to start reporting it.
bool vulkan_command_list::raytracing_is_supported() const
{
    return false;
}
} // namespace sg::backend::vulkan
