// vulkan_download_async: GPU->CPU transfer on the transfer queue, decoupled from epochs (ctx.download).
// See libs/graphics/shaped-graphics/docs/concepts/download.async.md.

#include <clean-core/common/assert.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_download_async.hh>

namespace sg::backend::vulkan
{
void vulkan_download_async_actor::on_message(vulkan_async_download_job job)
{
    _system.process(job);
}

cc::result<cc::unit> vulkan_download_async_system::initialize(vulkan_context& ctx, isize window_bytes)
{
    CC_ASSERT(window_bytes > 0, "the async download window must be positive");
    _ctx = &ctx;
    _window_bytes = window_bytes;

    auto const type_info = VkSemaphoreTypeCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    auto const semaphore_info
        = VkSemaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type_info};
    if (VkResult const r = vkCreateSemaphore(ctx._device, &semaphore_info, nullptr, &_window_timeline); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateSemaphore (async download windows) failed");

    auto const pool_info = VkCommandPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = ctx.transfer_queue_family(),
    };
    for (int i = 0; i < k_window_count; ++i)
    {
        if (VkResult const r = vkCreateCommandPool(ctx._device, &pool_info, nullptr, &_window_pools[i]); r != VK_SUCCESS)
            return vulkan_error(r, "vkCreateCommandPool (async download) failed");

        auto const alloc = VkCommandBufferAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = _window_pools[i],
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (VkResult const r = vkAllocateCommandBuffers(ctx._device, &alloc, &_window_buffers[i]); r != VK_SUCCESS)
            return vulkan_error(r, "vkAllocateCommandBuffers (async download) failed");
    }

    auto const buffer_info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(window_bytes * isize(k_window_count)),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, // only the transfer queue ever writes it
    };
    if (VkResult const r = vkCreateBuffer(ctx._device, &buffer_info, nullptr, &_staging); r != VK_SUCCESS)
        return vulkan_error(r, "vkCreateBuffer (async download staging) failed");

    VkMemoryRequirements req = {};
    vkGetBufferMemoryRequirements(ctx._device, _staging, &req);
    u32 const type = ctx.find_memory_type(u32(req.memoryTypeBits),
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX)
        return cc::error("no host-visible memory type for the async download staging buffer");

    auto const alloc = VkMemoryAllocateInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    if (VkResult const r = vkAllocateMemory(ctx._device, &alloc, nullptr, &_staging_memory); r != VK_SUCCESS)
        return vulkan_error(r, "vkAllocateMemory (async download staging) failed");
    if (VkResult const r = vkBindBufferMemory(ctx._device, _staging, _staging_memory, 0); r != VK_SUCCESS)
        return vulkan_error(r, "vkBindBufferMemory (async download staging) failed");

    void* mapped = nullptr;
    if (VkResult const r = vkMapMemory(ctx._device, _staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped); r != VK_SUCCESS)
        return vulkan_error(r, "vkMapMemory (async download staging) failed");
    _mapped = static_cast<byte*>(mapped);

    _actor = cc::make_and_start_threaded_actor<vulkan_download_async_actor>(*this);
    return cc::unit{};
}

void vulkan_download_async_system::wait_for_window(int slot)
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

void vulkan_download_async_system::process(vulkan_async_download_job& job)
{
    CC_RECORD_SCOPE("sg.download.async.copy");

    auto const source = job.buffer_source.lock();
    bool const wanted = job.pin.lock() != nullptr;

    // A source dropped mid-flight, or a caller that dropped the future, is a cancellation rather than a delivery:
    // the bytes were never written anywhere the caller can see.
    // The completion value is signaled either way, so a later writer waiting on it never hangs — which is the whole
    // point of reserving it at enqueue.
    // `signal_here` is false on the delivered path: the last chunk's submit already signalled the completion value
    // on the GPU, and signalling it again on the host is an error rather than a no-op — a timeline may only ever
    // move forwards.
    auto const settle = [&](bool delivered, bool signal_here)
    {
        if (signal_here && job.completion_value.is_pending())
        {
            auto const signal = VkSemaphoreSignalInfo{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                .semaphore = job.completion_value.group->timeline,
                .value = job.completion_value.value,
            };
            vkSignalSemaphore(_ctx->_device, &signal);
        }
        if (job.completion)
        {
            if (delivered)
                job.completion->push_value(cc::unit{});
            else
                job.completion->push_error(cc::async_error::make_cancelled());
        }
    };

    if (source == nullptr || !wanted || job.size_in_bytes == 0 || source->_buffer == VK_NULL_HANDLE)
    {
        // Nothing was queued, so the value has to be signalled here or a later writer waiting on it hangs.
        settle(false, /*signal_here =*/true);
        return;
    }

    // A readback larger than a window is delivered window by window: each chunk is copied, waited for and memcpy'd
    // out before the next reuses the staging.
    isize done = 0;
    while (done < job.size_in_bytes)
    {
        int const slot = _next_window;
        _next_window = (_next_window + 1) % k_window_count;
        wait_for_window(slot);

        auto const chunk = cc::min(_window_bytes, job.size_in_bytes - done);

        vkResetCommandPool(_ctx->_device, _window_pools[slot], 0);
        auto const begin = VkCommandBufferBeginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(_window_buffers[slot], &begin);
        auto const region = VkBufferCopy{
            .srcOffset = VkDeviceSize(job.src_offset + done),
            .dstOffset = VkDeviceSize(isize(slot) * _window_bytes),
            .size = VkDeviceSize(chunk),
        };
        vkCmdCopyBuffer(_window_buffers[slot], source->_buffer, _staging, 1, &region);
        vkEndCommandBuffer(_window_buffers[slot]);

        done += chunk;
        bool const is_last = done >= job.size_in_bytes;

        // Two edges, both into this one submit: the graphics token this read was deferred behind, and any async
        // upload to the same buffer that has not landed.
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
        if (job.upload_wait.is_pending() && !job.upload_wait.has_reached())
        {
            waits[wait_count] = job.upload_wait.group->timeline;
            wait_values[wait_count] = job.upload_wait.value;
            ++wait_count;
        }

        ++_window_next_value;
        _window_values[slot] = _window_next_value;

        // The completion value is signaled with the LAST chunk, so a later writer waiting on it waits for the whole
        // readback rather than for its first window.
        VkSemaphore signals[2] = {_window_timeline, VK_NULL_HANDLE};
        u64 signal_values[2] = {_window_next_value, 0};
        u32 signal_count = 1;
        if (is_last && job.completion_value.is_pending())
        {
            signals[1] = job.completion_value.group->timeline;
            signal_values[1] = job.completion_value.value;
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
            [&](int&) { return vkQueueSubmit(_ctx->download_queue(), 1, &submit, VK_NULL_HANDLE); });
        CC_ASSERT(r == VK_SUCCESS, "vkQueueSubmit (async download) failed");

        // Yield before blocking.
        // Where this actor has no thread of its own it runs on whoever swept the pump registry, so blocking here
        // would stall every other cooperative worker along with it — including whatever the copy is waiting on.
        u64 current = 0;
        vkGetSemaphoreCounterValue(_ctx->_device, _window_timeline, &current);
        while (current < _window_values[slot] && cc::thread_pump_all())
            vkGetSemaphoreCounterValue(_ctx->_device, _window_timeline, &current);
        wait_for_window(slot);

        // Only now are the bytes in host memory.
        cc::memcpy(job.destination.data() + done - chunk, _mapped + isize(slot) * _window_bytes, size_t(chunk));
    }

    settle(true, /*signal_here =*/false);
}

sg::bytes_future vulkan_download_async_system::download_buffer(sg::raw_buffer_handle const& buffer,
                                                               isize offset,
                                                               isize size_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "async download source buffer is null");
    auto const src = std::dynamic_pointer_cast<vulkan_buffer const>(buffer);
    CC_ASSERT(src != nullptr, "buffer is not a vulkan buffer");
    CC_ASSERT(!src->is_expired(), "async download source is a transient buffer used past its epoch (expired)");
    CC_ASSERT(offset >= 0 && size_in_bytes >= 0 && offset + size_in_bytes <= src->size_in_bytes(),
              "async download range is out of the buffer's bounds");
    // Ready on construction, with an empty value: there is no GPU work to wait for, and a default-constructed
    // future would report not-ready forever.
    if (size_in_bytes == 0)
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_ready_completion(), nullptr);
    CC_ASSERT(src->usage().has(sg::buffer_usage::copy_src), "async download source buffer must have "
                                                            "buffer_usage::copy_src");
    CC_ASSERT(src->_download_group != nullptr, "a copy_src buffer must carry a download timeline");

    auto dst = cc::pinned_data<byte>::create_uninitialized(size_in_bytes);
    auto completion = cc::make_async_manual<cc::unit>();

    vulkan_async_download_job job;
    job.buffer_source = src;
    job.src_offset = offset;
    job.size_in_bytes = size_in_bytes;
    job.destination = dst.span();
    job.pin = std::weak_ptr<void const>(dst.pin());
    job.completion = completion;
    job.completion_value = {.group = src->_download_group, .value = src->_download_group->reserve()};

    // Forward: a later graphics-queue list that WRITES this buffer waits for the readback, so it never overwrites
    // bytes still being read.
    // Reverse: the readback defers behind the last list that used the buffer, so it reads what that list left.
    src->_pending_async_download_value.store(job.completion_value.value, cc::memory_order_release);
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(cc::memory_order_acquire));
    if (auto const pending = src->_pending_async_upload_value.load(cc::memory_order_acquire); pending != 0)
        job.upload_wait = {.group = src->_upload_group, .value = pending};

    _actor->enqueue_message(cc::move(job));

    // No wait gate: it exists to say whether blocking could make progress at all, and an async readback's progress
    // depends on the copy actor rather than on the caller submitting anything.
    // The inline path needs one because its copy is only queued when its command list is.
    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(dst)), cc::move(completion), nullptr);
}

void vulkan_download_async_system::shutdown()
{
    if (_ctx == nullptr)
        return;

    if (_actor)
    {
        _actor->shutdown(); // drains every queued readback, each of which waits for its own copy
        _actor = {};
    }
    for (int i = 0; i < k_window_count; ++i)
        wait_for_window(i);

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
