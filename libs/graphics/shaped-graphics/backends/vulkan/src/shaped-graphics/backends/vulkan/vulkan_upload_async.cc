// vulkan_upload_async: CPU->GPU transfer on the transfer queue, decoupled from epochs (ctx.upload).
// See libs/graphics/shaped-graphics/docs/concepts/upload.async.md.

#include <clean-core/common/assert.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_upload_async.hh>

namespace sg::backend::vulkan
{
void vulkan_upload_actor::on_message(vulkan_async_upload_job job)
{
    _system.process(job);
}

cc::result<cc::unit> vulkan_upload_async_system::initialize(vulkan_context& ctx, isize window_bytes)
{
    CC_ASSERT(window_bytes > 0, "the async upload window must be positive");
    _ctx = &ctx;
    _desired_window_bytes.store(window_bytes, cc::memory_order_relaxed);

    auto const type_info = VkSemaphoreTypeCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    auto const semaphore_info
        = VkSemaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type_info};
    if (VkResult const r = vkCreateSemaphore(ctx._device, &semaphore_info, nullptr, &_window_timeline); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateSemaphore (async upload windows) failed");

    // One pool per window, so resetting a window's recording never touches one still in flight.
    auto const pool_info = VkCommandPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = ctx.transfer_queue_family(),
    };
    for (int i = 0; i < k_window_count; ++i)
    {
        if (VkResult const r = vkCreateCommandPool(ctx._device, &pool_info, nullptr, &_window_pools[i]); r != VK_SUCCESS)
            return vulkan_error(r, "vkCreateCommandPool (async upload) failed");

        auto const alloc = VkCommandBufferAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = _window_pools[i],
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (VkResult const r = vkAllocateCommandBuffers(ctx._device, &alloc, &_window_buffers[i]); r != VK_SUCCESS)
            return vulkan_error(r, "vkAllocateCommandBuffers (async upload) failed");
    }

    CC_RETURN_IF_ERROR(build_staging(window_bytes));

    _actor = cc::make_and_start_threaded_actor<vulkan_upload_actor>(*this);
    return cc::unit{};
}

cc::result<cc::unit> vulkan_upload_async_system::build_staging(isize window_bytes)
{
    auto const total = window_bytes * isize(k_window_count);
    auto const buffer_info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(total),
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, // only the transfer queue ever reads it
    };
    if (VkResult const r = vkCreateBuffer(_ctx->_device, &buffer_info, nullptr, &_staging); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateBuffer (async upload staging) failed");

    VkMemoryRequirements req = {};
    vkGetBufferMemoryRequirements(_ctx->_device, _staging, &req);
    u32 const type = _ctx->find_memory_type(u32(req.memoryTypeBits),
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX)
        return cc::error("no host-visible memory type for the async upload staging buffer");

    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    if (VkResult const r = vkAllocateMemory(_ctx->_device, &alloc, nullptr, &_staging_memory); r != VK_SUCCESS)
        return vulkan_error(r, "vkAllocateMemory (async upload staging) failed");
    if (VkResult const r = vkBindBufferMemory(_ctx->_device, _staging, _staging_memory, 0); r != VK_SUCCESS)
        return vulkan_error(r, "vkBindBufferMemory (async upload staging) failed");

    void* mapped = nullptr;
    if (VkResult const r = vkMapMemory(_ctx->_device, _staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped); r != VK_SUCCESS)
        return vulkan_error(r, "vkMapMemory (async upload staging) failed");

    _mapped = static_cast<byte*>(mapped);
    _window_bytes = window_bytes;
    return cc::unit{};
}

void vulkan_upload_async_system::release_staging()
{
    if (_mapped != nullptr)
    {
        vkUnmapMemory(_ctx->_device, _staging_memory);
        _mapped = nullptr;
    }
    if (_staging != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(_ctx->_device, _staging, nullptr);
        _staging = VK_NULL_HANDLE;
    }
    if (_staging_memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(_ctx->_device, _staging_memory, nullptr);
        _staging_memory = VK_NULL_HANDLE;
    }
}

void vulkan_upload_async_system::set_window_bytes(isize bytes)
{
    CC_ASSERT(bytes > 0, "the async upload window must be positive");
    _desired_window_bytes.store(bytes, cc::memory_order_relaxed);
}

void vulkan_upload_async_system::wait_for_window(int slot)
{
    u64 const target = _window_values[slot];
    if (target == 0)
        return;

    u64 current = 0;
    vkGetSemaphoreCounterValue(_ctx->_device, _window_timeline, &current);
    if (current >= target)
        return;

    auto const wait = VkSemaphoreWaitInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores = &_window_timeline,
        .pValues = &target,
    };
    vkWaitSemaphores(_ctx->_device, &wait, UINT64_MAX);
}

void vulkan_upload_async_system::apply_pending_window_bytes()
{
    auto const desired = _desired_window_bytes.load(cc::memory_order_relaxed);
    if (desired == _window_bytes)
        return;

    // Every window has to be idle before staging can go, since a queued copy still reads it.
    for (int i = 0; i < k_window_count; ++i)
        wait_for_window(i);
    release_staging();
    (void)build_staging(desired);
    for (int i = 0; i < k_window_count; ++i)
        _window_values[i] = 0;
    _next_window = 0;
}

void vulkan_upload_async_system::process(vulkan_async_upload_job& job)
{
    CC_RECORD_SCOPE("sg.upload.async.stage");

    apply_pending_window_bytes();

    auto const target = job.buffer_target.lock();
    auto const total = job.src.size();

    // The destination is gone, or there is nothing to copy: signal the completion value anyway, so the lifetime gate
    // and any forward reader stamped with it never hang.
    // That is the whole reason the value is reserved at enqueue rather than at stage time.
    if (target == nullptr || total == 0 || target->_buffer == VK_NULL_HANDLE)
    {
        if (job.completion.is_pending())
        {
            auto const signal_info = VkSemaphoreSignalInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                .semaphore = job.completion.group->timeline,
                .value = job.completion.value,
            };
            vkSignalSemaphore(_ctx->_device, &signal_info);
        }
        return;
    }

    // An upload larger than a window packs across successive windows; only the last chunk signals completion, so a
    // reader waiting on it sees every byte.
    isize copied = 0;
    while (copied < total)
    {
        int const slot = _next_window;
        _next_window = (_next_window + 1) % k_window_count;
        wait_for_window(slot);

        auto const chunk = cc::min(_window_bytes, total - copied);
        cc::memcpy(_mapped + isize(slot) * _window_bytes, job.src.data() + copied, size_t(chunk));

        vkResetCommandPool(_ctx->_device, _window_pools[slot], 0);
        auto const begin = VkCommandBufferBeginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(_window_buffers[slot], &begin);
        auto const region = VkBufferCopy{
            .srcOffset = VkDeviceSize(isize(slot) * _window_bytes),
            .dstOffset = VkDeviceSize(job.dst_offset + copied),
            .size = VkDeviceSize(chunk),
        };
        vkCmdCopyBuffer(_window_buffers[slot], _staging, target->_buffer, 1, &region);
        vkEndCommandBuffer(_window_buffers[slot]);

        copied += chunk;
        bool const is_last = copied >= total;

        // The cross-queue handshake, both directions in one submit — which is what a timeline semaphore buys over
        // dx12's separate Wait and Signal calls on two queues.
        //
        // Wait: the graphics-queue token this upload was deferred behind, so it never overwrites bytes an
        // already-submitted list still reads.
        // Signal: this window's reuse value always, plus the destination's completion value on the last chunk.
        VkSemaphore waits[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        u64 wait_values[2] = {0, 0};
        VkPipelineStageFlags const wait_stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
        u32 wait_count = 0;
        if (job.wait_token != sg::submission_token::not_submitted)
        {
            waits[wait_count] = _ctx->_submission_timeline;
            wait_values[wait_count] = u64(job.wait_token);
            ++wait_count;
        }
        if (job.download_wait.is_pending() && !job.download_wait.has_reached())
        {
            waits[wait_count] = job.download_wait.group->timeline;
            wait_values[wait_count] = job.download_wait.value;
            ++wait_count;
        }

        ++_window_next_value;
        _window_values[slot] = _window_next_value;

        VkSemaphore signals[2] = {_window_timeline, VK_NULL_HANDLE};
        u64 signal_values[2] = {_window_next_value, 0};
        u32 signal_count = 1;
        if (is_last && job.completion.is_pending())
        {
            signals[1] = job.completion.group->timeline;
            signal_values[1] = job.completion.value;
            signal_count = 2;
        }

        auto const timeline_info = VkTimelineSemaphoreSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = wait_count,
            .pWaitSemaphoreValues = wait_count != 0 ? wait_values : nullptr,
            .signalSemaphoreValueCount = signal_count,
            .pSignalSemaphoreValues = signal_values,
        };
        auto const submit = VkSubmitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &timeline_info,
            .waitSemaphoreCount = wait_count,
            .pWaitSemaphores = wait_count != 0 ? waits : nullptr,
            .pWaitDstStageMask = wait_count != 0 ? wait_stages : nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &_window_buffers[slot],
            .signalSemaphoreCount = signal_count,
            .pSignalSemaphores = signals,
        };
        // Vulkan queues are externally synchronized — see vulkan_context::queue_guard.
        VkResult const r = _ctx->queue_guard().lock(
            [&](int&) { return vkQueueSubmit(_ctx->upload_queue(), 1, &submit, VK_NULL_HANDLE); });
        CC_ASSERT(r == VK_SUCCESS, "vkQueueSubmit (async upload) failed");
    }
}

void vulkan_upload_async_system::upload_buffer(sg::raw_buffer_handle const& buffer,
                                               cc::pinned_data<byte const> data,
                                               isize offset)
{
    // The scope forwards straight here without validating, so the contract is checked at this seam.
    CC_ASSERT(buffer != nullptr, "async upload target buffer is null");
    auto const dst = std::dynamic_pointer_cast<vulkan_buffer const>(buffer);
    CC_ASSERT(dst != nullptr, "buffer is not a vulkan buffer");
    CC_ASSERT(!dst->is_expired(), "async upload target is a transient buffer used past its epoch (expired)");
    CC_ASSERT(offset >= 0 && offset + data.size() <= dst->size_in_bytes(), "async upload range is out of the "
                                                                           "buffer's bounds");
    if (data.empty())
        return;
    CC_ASSERT(dst->usage().has(sg::buffer_usage::copy_dst), "async upload target buffer must have "
                                                            "buffer_usage::copy_dst");
    CC_ASSERT(dst->_upload_group != nullptr, "a copy_dst buffer must carry an upload timeline");

    vulkan_async_upload_job job;
    job.buffer_target = dst;
    job.dst_offset = offset;
    job.src = cc::move(data);

    // Reserved here, on the caller's thread, so completion values are ordered the way the jobs were handed over —
    // which is the order the actor then preserves.
    job.completion = {.group = dst->_upload_group, .value = dst->_upload_group->reserve()};

    // Forward: a later graphics-queue list touching this buffer waits for this value at submit.
    // Reverse: captured now rather than at stage time, so a list submitted after this cannot be waited on by it.
    dst->_pending_async_upload_value.store(job.completion.value, cc::memory_order_release);
    job.wait_token = sg::submission_token(dst->_last_used_submission_token.load(cc::memory_order_acquire));
    if (auto const pending = dst->_pending_async_download_value.load(cc::memory_order_acquire); pending != 0)
        job.download_wait = {.group = dst->_download_group, .value = pending};

    _actor->enqueue_message(cc::move(job));
}

void vulkan_upload_async_system::shutdown()
{
    if (_ctx == nullptr)
        return;

    if (_actor)
    {
        _actor->shutdown(); // drains every queued copy first
        _actor = {};
    }

    // Every submitted copy must be done before the staging it reads goes.
    for (int i = 0; i < k_window_count; ++i)
        wait_for_window(i);

    release_staging();
    for (int i = 0; i < k_window_count; ++i)
        if (_window_pools[i] != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(_ctx->_device, _window_pools[i], nullptr);
            _window_pools[i] = VK_NULL_HANDLE;
        }
    if (_window_timeline != VK_NULL_HANDLE)
    {
        vkDestroySemaphore(_ctx->_device, _window_timeline, nullptr);
        _window_timeline = VK_NULL_HANDLE;
    }
    _ctx = nullptr;
}
} // namespace sg::backend::vulkan
