// vulkan_download_async: GPU->CPU transfer on the transfer queue, decoupled from epochs (ctx.download).
// See libs/graphics/shaped-graphics/docs/concepts/download.async.md.

#include <clean-core/common/assert.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/platform/leak_annotations.hh>
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/backends/vulkan/vulkan_barrier.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_download_async.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>
#include <shaped-graphics/resource/pixel_format.hh>

namespace sg::backend::vulkan
{
void vulkan_download_async_actor::on_thread_init()
{
    _system.warm_up_driver_thread();
}

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

void vulkan_download_async_system::warm_up_driver_thread()
{
    if (_ctx == nullptr || _window_timeline == VK_NULL_HANDLE)
        return;

    cc::leak_scope const driver_thread_init;
    u64 value = 0;
    vkGetSemaphoreCounterValue(_ctx->_device, _window_timeline, &value);
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
    auto const texture = job.texture_source.lock();
    bool const alive = job.is_texture ? texture != nullptr : source != nullptr;
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
        // An empty submit rather than a host signal: a timeline rejects a host signal that would overtake a queued
        // one, and values are reserved in enqueue order — so a skipped readback's value is higher than an earlier
        // one whose copy is still in flight.
        if (signal_here && job.completion_value.is_pending())
        {
            u64 const signal_value = job.completion_value.value;
            auto const timeline_info = VkTimelineSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .signalSemaphoreValueCount = 1,
                .pSignalSemaphoreValues = &signal_value,
            };
            auto const empty_submit = VkSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = &timeline_info,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &job.completion_value.group->timeline,
            };
            (void)_ctx->queue_guard().lock(
                [&](int&) { return vkQueueSubmit(_ctx->download_queue(), 1, &empty_submit, VK_NULL_HANDLE); });
        }
        if (job.completion)
        {
            if (delivered)
                job.completion->push_value(cc::unit{});
            else
                job.completion->push_error(cc::async_error::make_cancelled());
        }
    };

    // A sink-driven readback has no resident destination, so a dropped future is not a cancellation there.
    bool const has_destination = job.sink ? true : wanted;
    if (!alive || !has_destination || job.size_in_bytes == 0)
    {
        // Nothing was queued, so the value has to be signalled here or a later writer waiting on it hangs.
        if (job.stream != nullptr && job.stream->completion != nullptr && !job.stream->completion->is_ready())
            job.stream->completion->push_error(cc::async_error::make_cancelled());
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

        // A texture copy addresses whole rows, so a texture chunk is clamped to a whole number of them — which is
        // also what makes a sink's chunks whole tightly-packed rows.
        auto chunk = cc::min(_window_bytes, job.size_in_bytes - done);
        if (job.is_texture && job.row_bytes > 0)
        {
            chunk = (chunk / job.row_bytes) * job.row_bytes;
            CC_ASSERT(chunk > 0, "the async download window is smaller than one texture row");
        }

        vkResetCommandPool(_ctx->_device, _window_pools[slot], 0);
        auto const begin = VkCommandBufferBeginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(_window_buffers[slot], &begin);

        if (job.is_texture)
        {
            // The transfer queue owns the layout here, as on the upload side — transition from whatever the graphics
            // side left, and hand the texture back in `general`, recorded on the tracker.
            auto const range = sg::subresource_range(job.subresource);
            auto const current = texture->canonical_layout_of(range);
            auto const sub_range = VkImageSubresourceRange{.aspectMask = vk_aspect_mask_from(range),
                                                           .baseMipLevel = u32(job.subresource.mip_level),
                                                           .levelCount = 1,
                                                           .baseArrayLayer = u32(job.subresource.array_layer),
                                                           .layerCount = 1};

            auto const to_copy = VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .oldLayout = vk_layout_from(current),
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = texture->_image,
                .subresourceRange = sub_range,
            };
            auto const dep = VkDependencyInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                              .imageMemoryBarrierCount = 1,
                                              .pImageMemoryBarriers = &to_copy};
            vkCmdPipelineBarrier2(_window_buffers[slot], &dep);

            auto const first_row = done / job.row_bytes;
            auto const row_count = chunk / job.row_bytes;
            auto const copy = VkBufferImageCopy{
                .bufferOffset = VkDeviceSize(isize(slot) * _window_bytes),
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource = {.aspectMask = vk_aspect_mask_from(range),
                                     .mipLevel = u32(job.subresource.mip_level),
                                     .baseArrayLayer = u32(job.subresource.array_layer),
                                     .layerCount = 1},
                .imageOffset = {job.region.offset[0], job.region.offset[1] + int(first_row), job.region.offset[2]},
                .imageExtent = {u32(job.region.size[0]), u32(row_count), u32(job.region.size[2])},
            };
            vkCmdCopyImageToBuffer(_window_buffers[slot], texture->_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   _staging, 1, &copy);

            auto const to_general = VkImageMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                .srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = texture->_image,
                .subresourceRange = sub_range,
            };
            auto const dep_back = VkDependencyInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                                   .imageMemoryBarrierCount = 1,
                                                   .pImageMemoryBarriers = &to_general};
            vkCmdPipelineBarrier2(_window_buffers[slot], &dep_back);
            texture->set_canonical_layout(range, sg::texture_layout::general);
        }
        else
        {
            auto const region = VkBufferCopy{
                .srcOffset = VkDeviceSize(job.src_offset + done),
                .dstOffset = VkDeviceSize(isize(slot) * _window_bytes),
                .size = VkDeviceSize(chunk),
            };
            vkCmdCopyBuffer(_window_buffers[slot], source->_buffer, _staging, 1, &region);
        }
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
        auto const* const staged = _mapped + isize(slot) * _window_bytes;
        if (job.sink)
        {
            // Handed over rather than accumulated, and only for the duration of this call — the window is recycled a
            // few windows later.
            // A sink that refuses fails the transfer, and no further chunk is delivered.
            if (!job.sink(cc::span<byte const>(staged, chunk), done - chunk))
            {
                if (job.stream != nullptr && job.stream->completion != nullptr && !job.stream->completion->is_ready())
                    job.stream->completion->push_error(cc::async_error::make_error(cc::any_error("stream sink rejected "
                                                                                                 "the chunk")));
                if (job.completion)
                    job.completion->push_error(cc::async_error::make_cancelled());
                return; // the completion value was signalled by the submit above
            }
        }
        else
            cc::memcpy(job.destination.data() + done - chunk, staged, size_t(chunk));

        if (job.stream != nullptr)
        {
            job.stream->bytes_done.fetch_add(i64(chunk), std::memory_order_relaxed);

            // Cancellation bounds future work rather than undoing past work: chunks already recorded still run.
            if (job.stream->cancelled.load(std::memory_order_relaxed) && !is_last)
            {
                if (job.stream->completion != nullptr && !job.stream->completion->is_ready())
                    job.stream->completion->push_error(cc::async_error::make_cancelled());
                if (job.completion)
                    job.completion->push_error(cc::async_error::make_cancelled());
                return;
            }
        }
    }

    // The future settles BEFORE the stream control, and the order is load-bearing.
    // A caller waits on the handle's completion and then reads its future, so settling the control first leaves a
    // window where the transfer says it is done and the bytes are not there yet — a race a test loses only
    // intermittently, which is the worst kind to ship.
    settle(true, /*signal_here =*/false);
    if (job.stream != nullptr && job.stream->completion != nullptr && !job.stream->completion->is_ready())
        job.stream->completion->push_value(cc::unit{});
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

namespace
{
/// What every streaming readback shares: the control node, and the stamping that makes it the streaming tier.
[[nodiscard]] std::shared_ptr<sg::impl::stream_control> make_stream_control(std::shared_ptr<vulkan_buffer const> const& src,
                                                                            u64 value)
{
    auto control = std::make_shared<sg::impl::stream_control>();
    control->completion = cc::make_async_manual<cc::unit>();
    control->total_hint.store(-1, std::memory_order_relaxed);

    // promote_to_async's backend half: a list recorded after the call waits on this readback like any async one.
    control->on_promote = [src, value]
    {
        u64 previous = src->_pending_async_download_value.load(cc::memory_order_relaxed);
        while (previous < value
               && !src->_pending_async_download_value.compare_exchange_weak(previous, value, cc::memory_order_acq_rel,
                                                                            cc::memory_order_relaxed))
        {
        }
    };
    return control;
}
} // namespace

sg::stream_download_handle vulkan_download_async_system::stream_buffer(sg::raw_buffer_handle const& buffer,
                                                                       isize offset,
                                                                       isize size_in_bytes)
{
    return stream_to_sink_buffer(buffer, sg::stream_sink{}, offset, size_in_bytes);
}

sg::stream_download_handle vulkan_download_async_system::stream_to_sink_buffer(sg::raw_buffer_handle const& buffer,
                                                                               sg::stream_sink sink,
                                                                               isize offset,
                                                                               isize size_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "streaming download source buffer is null");
    auto const src = std::dynamic_pointer_cast<vulkan_buffer const>(buffer);
    CC_ASSERT(src != nullptr, "buffer is not a vulkan buffer");
    CC_ASSERT(!src->is_expired(), "streaming download source is a transient buffer used past its epoch (expired)");
    CC_ASSERT(offset >= 0 && size_in_bytes >= 0 && offset + size_in_bytes <= src->size_in_bytes(),
              "streaming download range is out of the buffer's bounds");
    CC_ASSERT(src->usage().has(sg::buffer_usage::copy_src), "streaming download source buffer must have "
                                                            "buffer_usage::copy_src");

    bool const has_sink = bool(sink);
    auto dst = has_sink ? cc::pinned_data<byte>() : cc::pinned_data<byte>::create_uninitialized(size_in_bytes);
    auto completion = cc::make_async_manual<cc::unit>();

    auto const value = src->_download_group->reserve();
    auto control = make_stream_control(src, value);
    control->total_hint.store(i64(size_in_bytes), std::memory_order_relaxed);

    // A sink-driven readback exposes no future: there is no resident destination, which is the whole point of
    // having a sink, so handing back a future over an empty buffer would be a lie the caller could act on.
    auto future = has_sink ? sg::bytes_future() : sg::bytes_future(cc::pinned_data<byte const>(dst), completion, nullptr);

    if (size_in_bytes == 0)
    {
        // Nothing to read: settle both nodes now rather than queueing an empty transfer.
        control->completion->push_value(cc::unit{});
        completion->push_value(cc::unit{});
        return sg::stream_download_handle(cc::move(control), cc::move(future));
    }

    vulkan_async_download_job job;
    job.buffer_source = src;
    job.src_offset = offset;
    job.size_in_bytes = size_in_bytes;
    job.destination = dst.span();
    job.pin = std::weak_ptr<void const>(dst.pin());
    job.completion = completion;
    job.completion_value = {.group = src->_download_group, .value = value};
    job.stream = control;
    job.sink = cc::move(sink);

    // The streaming tier stamps only the LIFETIME value: a later command list waits on nothing until promoted.
    src->_pending_stream_download_value.store(value, cc::memory_order_release);
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(cc::memory_order_acquire));
    if (auto const pending = src->_pending_async_upload_value.load(cc::memory_order_acquire); pending != 0)
        job.upload_wait = {.group = src->_upload_group, .value = pending};

    _actor->enqueue_message(cc::move(job));
    return sg::stream_download_handle(cc::move(control), cc::move(future));
}

namespace
{
/// Bytes per row of `region`, the granularity a texture readback's chunks fall on.
[[nodiscard]] isize region_row_bytes(sg::pixel_format format, sg::texture_region const& region)
{
    int const block_extent = sg::format_block_extent(format);
    int const block_size = sg::format_block_size(format);
    isize const blocks_x = (region.size[0] + block_extent - 1) / block_extent;
    return blocks_x * isize(block_size);
}

/// The tightly-packed size of `region`, which is what a readback delivers.
[[nodiscard]] isize region_size_bytes(sg::pixel_format format, sg::texture_region const& region)
{
    int const block_extent = sg::format_block_extent(format);
    isize const blocks_y = (region.size[1] + block_extent - 1) / block_extent;
    return region_row_bytes(format, region) * blocks_y * isize(region.size[2]);
}

void validate_texture_source(std::shared_ptr<vulkan_texture const> const& src)
{
    CC_ASSERT(src != nullptr, "texture is not a vulkan texture");
    CC_ASSERT(!src->is_expired(), "download source is a transient texture used past its epoch (expired)");
    CC_ASSERT(src->usage().has(sg::texture_usage::copy_src), "download source texture must have "
                                                             "texture_usage::copy_src");
    CC_ASSERT(src->_download_group != nullptr, "a copy_src texture must carry a download timeline");
}
} // namespace

sg::bytes_future vulkan_download_async_system::download_texture(sg::raw_texture_handle const& texture,
                                                                sg::subresource_index const& subresource,
                                                                sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "async download source texture is null");
    auto const src = std::dynamic_pointer_cast<vulkan_texture const>(texture);
    validate_texture_source(src);

    auto const format = src->description().format;
    auto const size_in_bytes = region_size_bytes(format, region);
    if (size_in_bytes == 0)
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_ready_completion(), nullptr);

    auto dst = cc::pinned_data<byte>::create_uninitialized(size_in_bytes);
    auto completion = cc::make_async_manual<cc::unit>();

    vulkan_async_download_job job;
    job.texture_source = src;
    job.is_texture = true;
    job.subresource = subresource;
    job.region = region;
    job.row_bytes = region_row_bytes(format, region);
    job.size_in_bytes = size_in_bytes;
    job.destination = dst.span();
    job.pin = std::weak_ptr<void const>(dst.pin());
    job.completion = completion;
    job.completion_value = {.group = src->_download_group, .value = src->_download_group->reserve()};

    src->_pending_async_download_value.store(job.completion_value.value, cc::memory_order_release);
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(cc::memory_order_acquire));
    if (auto const pending = src->_pending_async_upload_value.load(cc::memory_order_acquire); pending != 0)
        job.upload_wait = {.group = src->_upload_group, .value = pending};

    _actor->enqueue_message(cc::move(job));
    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(dst)), cc::move(completion), nullptr);
}

sg::stream_download_handle vulkan_download_async_system::stream_to_sink_texture(sg::raw_texture_handle const& texture,
                                                                                sg::stream_sink sink,
                                                                                sg::subresource_index const& subresource,
                                                                                sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "streaming download source texture is null");
    auto const src = std::dynamic_pointer_cast<vulkan_texture const>(texture);
    validate_texture_source(src);

    auto const format = src->description().format;
    auto const size_in_bytes = region_size_bytes(format, region);
    bool const has_sink = bool(sink);

    auto dst = has_sink ? cc::pinned_data<byte>() : cc::pinned_data<byte>::create_uninitialized(size_in_bytes);
    auto completion = cc::make_async_manual<cc::unit>();

    auto const value = src->_download_group->reserve();
    auto control = std::make_shared<sg::impl::stream_control>();
    control->completion = cc::make_async_manual<cc::unit>();
    control->total_hint.store(i64(size_in_bytes), std::memory_order_relaxed);
    control->on_promote = [src, value]
    {
        u64 previous = src->_pending_async_download_value.load(cc::memory_order_relaxed);
        while (previous < value
               && !src->_pending_async_download_value.compare_exchange_weak(previous, value, cc::memory_order_acq_rel,
                                                                            cc::memory_order_relaxed))
        {
        }
    };

    auto future = has_sink ? sg::bytes_future() : sg::bytes_future(cc::pinned_data<byte const>(dst), completion, nullptr);

    if (size_in_bytes == 0)
    {
        control->completion->push_value(cc::unit{});
        completion->push_value(cc::unit{});
        return sg::stream_download_handle(cc::move(control), cc::move(future));
    }

    vulkan_async_download_job job;
    job.texture_source = src;
    job.is_texture = true;
    job.subresource = subresource;
    job.region = region;
    job.row_bytes = region_row_bytes(format, region);
    job.size_in_bytes = size_in_bytes;
    job.destination = dst.span();
    job.pin = std::weak_ptr<void const>(dst.pin());
    job.completion = completion;
    job.completion_value = {.group = src->_download_group, .value = value};
    job.stream = control;
    job.sink = cc::move(sink);

    src->_pending_stream_download_value.store(value, cc::memory_order_release);
    job.wait_token = sg::submission_token(src->_last_used_submission_token.load(cc::memory_order_acquire));
    if (auto const pending = src->_pending_async_upload_value.load(cc::memory_order_acquire); pending != 0)
        job.upload_wait = {.group = src->_upload_group, .value = pending};

    _actor->enqueue_message(cc::move(job));
    return sg::stream_download_handle(cc::move(control), cc::move(future));
}
} // namespace sg::backend::vulkan
