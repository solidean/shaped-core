// vulkan_upload_async: CPU->GPU transfer on the transfer queue, decoupled from epochs.
// Serves both ctx.upload (async tier) and ctx.stream's upload half (streaming tier) — see the header for why one
// system covers both.
// See libs/graphics/shaped-graphics/docs/concepts/upload.async.md.

#include <clean-core/common/assert.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/common/utility.hh> // cc::memcpy
#include <clean-core/platform/leak_annotations.hh>
#include <shaped-graphics/backends/vulkan/vulkan_barrier.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_context.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>
#include <shaped-graphics/backends/vulkan/vulkan_upload_async.hh>
#include <shaped-graphics/resource/pixel_format.hh>

namespace sg::backend::vulkan
{
void vulkan_upload_waker::wake()
{
    _target.lock(
        [](target& t)
        {
            if (t.system != nullptr)
                t.system->wake_actor();
        });
}

void vulkan_upload_actor::on_thread_init()
{
    _system.warm_up_driver_thread();
}

void vulkan_upload_actor::on_message(vulkan_async_upload_job job)
{
    _system.admit(cc::move(job));
}

void vulkan_upload_actor::on_message(vulkan_transfer_wake)
{
    // Nothing to do: arriving at all is the point, and on_process runs right after.
}

bool vulkan_upload_actor::on_process()
{
    _system.settle_finished();
    if (_system.run_one_window())
        return true;

    // Nothing could be staged — every job is stalled, or they are all done.
    // Outstanding settlements are waited for rather than slept on, since the actor may get no further message.
    return _system.wait_and_settle();
}

// --- setup ---------------------------------------------------------------------------------------

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
    _scheduler.set_window_bytes(window_bytes);

    _waker = std::make_shared<vulkan_upload_waker>(*this);
    _actor = cc::make_and_start_threaded_actor<vulkan_upload_actor>(*this);
    return cc::unit{};
}

cc::result<cc::unit> vulkan_upload_async_system::build_staging(isize window_bytes)
{
    auto const buffer_info = VkBufferCreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = VkDeviceSize(window_bytes * isize(k_window_count)),
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

void vulkan_upload_async_system::warm_up_driver_thread()
{
    if (_ctx == nullptr || _window_timeline == VK_NULL_HANDLE)
        return;

    cc::leak_scope const driver_thread_init;
    u64 value = 0;
    vkGetSemaphoreCounterValue(_ctx->_device, _window_timeline, &value);
}

void vulkan_upload_async_system::wake_actor()
{
    if (_actor)
        _actor->enqueue_message(vulkan_transfer_wake{});
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

    for (int i = 0; i < k_window_count; ++i)
        wait_for_window(i);
    release_staging();
    (void)build_staging(desired);
    _scheduler.set_window_bytes(desired);
    for (int i = 0; i < k_window_count; ++i)
        _window_values[i] = 0;
    _next_window = 0;
}

// --- settlement ----------------------------------------------------------------------------------

void vulkan_upload_async_system::signal_on_queue(vulkan_group_value const& value)
{
    if (!value.is_pending())
        return;

    u64 const signal_value = value.value;
    auto const timeline_info = VkTimelineSemaphoreSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount = 1,
        .pSignalSemaphoreValues = &signal_value,
    };
    auto const submit = VkSubmitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &timeline_info,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &value.group->timeline,
    };
    (void)_ctx->queue_guard().lock([&](int&) { return vkQueueSubmit(_ctx->upload_queue(), 1, &submit, VK_NULL_HANDLE); });
}

void vulkan_upload_async_system::settle_now(vulkan_async_upload_job& job, bool delivered)
{
    if (job.stream == nullptr || job.stream->completion == nullptr)
        return;
    if (job.stream->completion->is_ready())
        return; // already settled; settling twice would be pushing to a resolved node
    if (delivered)
        job.stream->completion->push_value(cc::unit{});
    else
        job.stream->completion->push_error(cc::async_error::make_cancelled());
}

void vulkan_upload_async_system::settle_finished()
{
    if (_awaiting.empty())
        return;

    u64 landed = 0;
    vkGetSemaphoreCounterValue(_ctx->_device, _window_timeline, &landed);

    for (isize i = 0; i < _awaiting.size();)
    {
        if (_awaiting[i].window_value > landed)
        {
            ++i;
            continue;
        }
        auto entry = cc::move(_awaiting[i]);
        _awaiting.remove_at_range({.offset = i, .size = 1});

        if (entry.stream != nullptr && entry.stream->completion != nullptr && !entry.stream->completion->is_ready())
        {
            if (entry.delivered)
                entry.stream->completion->push_value(cc::unit{});
            else
                entry.stream->completion->push_error(cc::async_error::make_cancelled());
        }
    }
}

bool vulkan_upload_async_system::wait_and_settle()
{
    if (_awaiting.empty())
        return false;

    u64 const target = _awaiting[0].window_value;
    u64 landed = 0;
    vkGetSemaphoreCounterValue(_ctx->_device, _window_timeline, &landed);
    if (landed < target)
    {
        auto const wait = VkSemaphoreWaitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &_window_timeline,
            .pValues = &target,
        };
        vkWaitSemaphores(_ctx->_device, &wait, UINT64_MAX);
    }
    settle_finished();
    return true;
}

// --- the window cycle ----------------------------------------------------------------------------

void vulkan_upload_async_system::admit(vulkan_async_upload_job job)
{
    // A source is polled on this thread from here on, so its waker is installed now.
    if (job.source != nullptr && _waker != nullptr)
        job.source->set_waker([waker = _waker] { waker->wake(); });
    _pending.push_back(cc::move(job));
}

bool vulkan_upload_async_system::run_one_window()
{
    if (_pending.empty())
        return false;

    apply_pending_window_bytes();

    // Rebuild the scheduler's view of the pending jobs.
    //
    // A cancelled or dead-destination job is retired here rather than skipped: leaving it pending would keep the
    // actor awake forever, and its completion node has to be settled on this path exactly as on any other.
    for (isize i = 0; i < _pending.size();)
    {
        auto& job = _pending[i];
        bool const cancelled = job.stream != nullptr && job.stream->cancelled.load(cc::memory_order_relaxed);
        bool const dead = job.buffer_target.expired() && job.texture_target.expired();
        if (!cancelled && !dead)
        {
            ++i;
            continue;
        }

        // Only a family HEAD may retire, even though it is going nowhere.
        //
        // Retiring signals its completion value, and every job of one family shares one timeline whose values were
        // reserved in sequence order — so retiring a later one first would signal a higher value before a lower one,
        // which a timeline rejects outright.
        // It waits one more cycle instead, by which time whatever was ahead of it has finished.
        bool is_head = true;
        for (isize j = 0; j < _pending.size(); ++j)
            if (j != i && _pending[j].family == job.family && _pending[j].sequence < job.sequence)
                is_head = false;
        if (!is_head)
        {
            ++i;
            continue;
        }

        // The completion value is signalled either way, so the lifetime gate and any forward reader stamped with it
        // never hang — which is the whole reason it is reserved at enqueue rather than at stage time.
        signal_on_queue(job.completion);
        settle_now(job, /*delivered =*/false);
        _pending.remove_at_range({.offset = i, .size = 1});
    }
    if (_pending.empty())
        return false;

    // One window, but several picks may be needed: a source that answers `not_yet` costs itself this window rather
    // than the system a thread, so it is marked ineligible and the scheduler is asked again.
    // Passing a stalled transfer over instead of queueing everything behind it is the whole reason `not_yet` is a
    // distinct answer rather than a blocking read.
    _scheduler.begin_window();
    auto stalled = cc::vector<bool>::create_filled(_pending.size(), false);

    isize index = -1;
    while (true)
    {
        cc::vector<sg::impl::transfer_candidate> candidates;
        for (isize i = 0; i < _pending.size(); ++i)
        {
            auto const& job = _pending[i];
            candidates.push_back({
                .flavor = job.stream != nullptr ? sg::impl::transfer_flavor::streaming : sg::impl::transfer_flavor::async,
                .priority = job.stream != nullptr ? job.stream->priority.load(cc::memory_order_relaxed) : 0,
                .age_seconds = 0,
                .family = job.family,
                .sequence = job.sequence,
                .eligible = !stalled[i],
            });
        }

        auto const picked = _scheduler.pick_next(candidates);
        if (!picked.has_value())
            return false;

        auto& candidate = _pending[picked.value()];

        // A source-driven transfer needs a chunk in hand before a window can be filled with it.
        if (candidate.source != nullptr && candidate.chunk.empty() && !candidate.source_done)
        {
            auto poll = candidate.source->try_next_chunk();
            if (poll.status == sg::stream_source_status::not_yet)
            {
                stalled[picked.value()] = true;
                continue;
            }
            if (poll.status == sg::stream_source_status::failed)
            {
                // The only way out for a source that can never produce what it promised — without it the transfer
                // would sit here forever, and anything chained onto its completion with it.
                signal_on_queue(candidate.completion);
                if (candidate.stream != nullptr && candidate.stream->completion != nullptr
                    && !candidate.stream->completion->is_ready())
                    candidate.stream->completion->push_error(cc::async_error::make_error(cc::any_error("stream source "
                                                                                                       "failed")));
                _pending.remove_at_range({.offset = picked.value(), .size = 1});
                return true;
            }
            if (poll.status == sg::stream_source_status::ready)
            {
                candidate.chunk = cc::move(poll.chunk.data);
                candidate.chunk_offset = poll.chunk.offset;
                candidate.staged = 0;
            }
            else
                candidate.source_done = true;
        }

        index = picked.value();
        break;
    }

    auto& job = _pending[index];

    auto const target = job.buffer_target.lock();
    auto const texture = job.texture_target.lock();
    bool const alive = job.is_texture ? texture != nullptr : target != nullptr;
    auto const payload = job.source != nullptr ? job.chunk.span() : job.src.span();
    auto const remaining = payload.size() - job.staged;

    // Nothing left to move: the last chunk has been staged, or the source said done.
    // The completion value was signalled by the last submit, so settlement only waits for that to land.
    if (!alive || (remaining == 0 && (job.source == nullptr || job.source_done)))
    {
        // A source-driven transfer's completion value could not ride its last submit — it is not known to be the
        // last until the source says `done`, one poll later — so it is queued here instead.
        // Right here rather than once the copy lands: anything submitted in between would signal a higher value
        // first, which a timeline rejects.
        if (job.source != nullptr)
            signal_on_queue(job.completion);

        if (job.last_window_value == 0)
        {
            if (job.source == nullptr)
                signal_on_queue(job.completion); // nothing was ever queued
            settle_now(job, /*delivered =*/alive);
        }
        else
            _awaiting.push_back({.window_value = job.last_window_value, .stream = job.stream, .delivered = true});
        _pending.remove_at_range({.offset = index, .size = 1});
        return true;
    }

    if (remaining == 0)
    {
        // A source chunk is fully staged; drop it and poll again next cycle.
        job.chunk = {};
        job.staged = 0;
        return true;
    }

    int const slot = _next_window;
    _next_window = (_next_window + 1) % k_window_count;
    wait_for_window(slot);

    // A texture copy places whole ROWS, the smallest unit it can address, so a texture chunk is clamped to a whole
    // number of them.
    // sg already requires a source's chunks to fall on row boundaries; this is the window doing the same.
    auto chunk_bytes = cc::min(_window_bytes, remaining);
    if (job.is_texture && job.row_bytes > 0)
    {
        chunk_bytes = (chunk_bytes / job.row_bytes) * job.row_bytes;
        CC_ASSERT(chunk_bytes > 0, "the async upload window is smaller than one texture row");
    }
    auto const dst_offset = job.dst_offset + (job.source != nullptr ? job.chunk_offset : 0) + job.staged;
    cc::memcpy(_mapped + isize(slot) * _window_bytes, payload.data() + job.staged, size_t(chunk_bytes));

    vkResetCommandPool(_ctx->_device, _window_pools[slot], 0);
    auto const begin = VkCommandBufferBeginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(_window_buffers[slot], &begin);

    if (job.is_texture)
    {
        // The transfer queue owns the layout here, which the per-list declare/flush rhythm cannot reach: it records
        // on the graphics queue.
        // So the image is transitioned from whatever the graphics side left it in, and handed back in `general` —
        // recorded on the tracker, so a later list transitions from the truth rather than from a stale canonical.
        auto const range = sg::subresource_range(job.subresource);
        auto const current = texture->canonical_layout_of(range);

        auto const to_copy = VkImageMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .oldLayout = vk_layout_from(current),
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture->_image,
            .subresourceRange = {.aspectMask = vk_aspect_mask_from(range, texture->format()),
                                 .baseMipLevel = u32(job.subresource.mip_level),
                                 .levelCount = 1,
                                 .baseArrayLayer = u32(job.subresource.array_layer),
                                 .layerCount = 1},
        };
        auto const dep = VkDependencyInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .imageMemoryBarrierCount = 1,
                                          .pImageMemoryBarriers = &to_copy};
        vkCmdPipelineBarrier2(_window_buffers[slot], &dep);

        // Which rows of the region this chunk covers.
        auto const first_row = dst_offset / job.row_bytes;
        auto const row_count = chunk_bytes / job.row_bytes;
        auto const copy = VkBufferImageCopy{
            .bufferOffset = VkDeviceSize(isize(slot) * _window_bytes),
            .bufferRowLength = 0, // tightly packed to imageExtent, which is what sg hands over
            .bufferImageHeight = 0,
            .imageSubresource = {.aspectMask = vk_aspect_mask_from(range, texture->format()),
                                 .mipLevel = u32(job.subresource.mip_level),
                                 .baseArrayLayer = u32(job.subresource.array_layer),
                                 .layerCount = 1},
            .imageOffset = {job.region.offset[0], job.region.offset[1] + int(first_row), job.region.offset[2]},
            .imageExtent = {u32(job.region.size[0]), u32(row_count), u32(job.region.size[2])},
        };
        vkCmdCopyBufferToImage(_window_buffers[slot], _staging, texture->_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy);

        auto const to_general = VkImageMemoryBarrier2{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = texture->_image,
            .subresourceRange = to_copy.subresourceRange,
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
            .srcOffset = VkDeviceSize(isize(slot) * _window_bytes),
            .dstOffset = VkDeviceSize(dst_offset),
            .size = VkDeviceSize(chunk_bytes),
        };
        vkCmdCopyBuffer(_window_buffers[slot], _staging, target->_buffer, 1, &region);
    }
    vkEndCommandBuffer(_window_buffers[slot]);

    job.staged += chunk_bytes;
    bool const payload_done = job.staged >= payload.size();
    bool const transfer_done = payload_done && (job.source == nullptr || job.source_done);

    // The cross-queue handshake, every edge in one submit.
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

    // A resident payload knows its last chunk, so its completion value rides that submit.
    // A source-driven one does not, and settles off the window value instead — see job.last_window_value.
    VkSemaphore signals[2] = {_window_timeline, VK_NULL_HANDLE};
    u64 signal_values[2] = {_window_next_value, 0};
    u32 signal_count = 1;
    if (transfer_done && job.source == nullptr && job.completion.is_pending())
    {
        signals[1] = job.completion.group->timeline;
        signal_values[1] = job.completion.value;
        signal_count = 2;
    }
    job.last_window_value = _window_next_value;

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
    VkResult const r
        = _ctx->queue_guard().lock([&](int&) { return vkQueueSubmit(_ctx->upload_queue(), 1, &submit, VK_NULL_HANDLE); });
    CC_ASSERT(r == VK_SUCCESS, "vkQueueSubmit (async upload) failed");

    if (job.stream != nullptr)
        job.stream->bytes_done.fetch_add(i64(chunk_bytes), std::memory_order_relaxed);

    bool const streaming = job.stream != nullptr;
    _scheduler.on_window_submitted(streaming ? 0 : chunk_bytes, streaming ? chunk_bytes : 0);

    if (transfer_done)
    {
        if (streaming)
            _awaiting.push_back({.window_value = job.last_window_value, .stream = job.stream, .delivered = true});
        _pending.remove_at_range({.offset = index, .size = 1});
    }
    else if (payload_done && job.source != nullptr)
    {
        job.chunk = {};
        job.staged = 0;
    }
    return true;
}

// --- enqueue -------------------------------------------------------------------------------------

namespace
{
/// The checks every upload seam owes, since the scope forwards straight here without validating.
void validate_buffer_target(std::shared_ptr<vulkan_buffer const> const& dst, isize offset, isize size)
{
    CC_ASSERT(dst != nullptr, "buffer is not a vulkan buffer");
    CC_ASSERT(!dst->is_expired(), "upload target is a transient buffer used past its epoch (expired)");
    CC_ASSERT(offset >= 0 && offset + size <= dst->size_in_bytes(), "upload range is out of the buffer's bounds");
    CC_ASSERT(dst->usage().has(sg::buffer_usage::copy_dst), "upload target buffer must have buffer_usage::copy_dst");
    CC_ASSERT(dst->_upload_group != nullptr, "a copy_dst buffer must carry an upload timeline");
}
} // namespace

void vulkan_upload_async_system::upload_buffer(sg::raw_buffer_handle const& buffer,
                                               cc::pinned_data<byte const> data,
                                               isize offset)
{
    CC_ASSERT(buffer != nullptr, "async upload target buffer is null");
    auto const dst = std::dynamic_pointer_cast<vulkan_buffer const>(buffer);
    validate_buffer_target(dst, offset, data.size());
    if (data.empty())
        return;

    vulkan_async_upload_job job;
    job.buffer_target = dst;
    job.dst_offset = offset;
    job.src = cc::move(data);
    job.family = u64(reinterpret_cast<uintptr_t>(dst.get()));
    job.sequence = _next_sequence.fetch_add(1, cc::memory_order_relaxed);

    // Reserved on the caller's thread, so completion values are ordered the way the jobs were handed over — which is
    // the order the scheduler's family rule then preserves.
    job.completion = {.group = dst->_upload_group, .value = dst->_upload_group->reserve()};

    // Forward: a later graphics-queue list touching this buffer waits for this value at submit.
    dst->_pending_async_upload_value.store(job.completion.value, cc::memory_order_release);
    job.wait_token = sg::submission_token(dst->_last_used_submission_token.load(cc::memory_order_acquire));
    if (auto const pending = dst->_pending_async_download_value.load(cc::memory_order_acquire); pending != 0)
        job.download_wait = {.group = dst->_download_group, .value = pending};

    _actor->enqueue_message(cc::move(job));
}

sg::stream_upload_handle vulkan_upload_async_system::stream_buffer(sg::raw_buffer_handle const& buffer,
                                                                   cc::pinned_data<byte const> data,
                                                                   isize offset)
{
    return stream_source_buffer(buffer, sg::make_pinned_stream_source(cc::move(data)), offset);
}

sg::stream_upload_handle vulkan_upload_async_system::stream_source_buffer(sg::raw_buffer_handle const& buffer,
                                                                          std::unique_ptr<sg::stream_source> source,
                                                                          isize offset)
{
    CC_ASSERT(buffer != nullptr, "streaming upload target buffer is null");
    CC_ASSERT(source != nullptr, "a streaming upload needs a source");
    auto const dst = std::dynamic_pointer_cast<vulkan_buffer const>(buffer);
    validate_buffer_target(dst, offset, 0);

    auto control = std::make_shared<sg::impl::stream_control>();
    control->completion = cc::make_async_manual<cc::unit>();
    if (auto const hint = source->total_size_hint(); hint >= 0)
        control->total_hint.store(hint, std::memory_order_relaxed);

    vulkan_async_upload_job job;
    job.buffer_target = dst;
    job.dst_offset = offset;
    job.source = cc::move(source);
    job.stream = control;
    job.family = u64(reinterpret_cast<uintptr_t>(dst.get()));
    job.sequence = _next_sequence.fetch_add(1, cc::memory_order_relaxed);
    job.completion = {.group = dst->_upload_group, .value = dst->_upload_group->reserve()};

    // The streaming tier deliberately stamps only the LIFETIME value: a later command list waits on nothing, which is
    // the guarantee it trades away for its priority.
    // Deferred deletion still gates on it, so the buffer cannot go while a copy is queued.
    dst->_pending_stream_copy_value.store(job.completion.value, cc::memory_order_release);
    job.wait_token = sg::submission_token(dst->_last_used_submission_token.load(cc::memory_order_acquire));
    if (auto const pending = dst->_pending_async_download_value.load(cc::memory_order_acquire); pending != 0)
        job.download_wait = {.group = dst->_download_group, .value = pending};

    // promote_to_async's backend half: move this transfer's value onto the async stamp too, so a list recorded after
    // the call waits on it like any async upload.
    control->on_promote = [dst, value = job.completion.value]
    {
        u64 previous = dst->_pending_async_upload_value.load(cc::memory_order_relaxed);
        while (previous < value
               && !dst->_pending_async_upload_value.compare_exchange_weak(previous, value, cc::memory_order_acq_rel,
                                                                          cc::memory_order_relaxed))
        {
        }
    };

    _actor->enqueue_message(cc::move(job));
    return sg::stream_upload_handle(cc::move(control));
}

namespace
{
/// Bytes per row of `region`, which is the granularity a texture chunk and a staging window are both clamped to.
[[nodiscard]] isize region_row_bytes(sg::pixel_format format, sg::texture_region const& region)
{
    int const block_extent = sg::format_block_extent(format);
    int const block_size = sg::format_block_size(format);
    isize const blocks_x = (region.size[0] + block_extent - 1) / block_extent;
    return blocks_x * isize(block_size);
}

/// The checks every texture upload seam owes.
void validate_texture_target(std::shared_ptr<vulkan_texture const> const& dst)
{
    CC_ASSERT(dst != nullptr, "texture is not a vulkan texture");
    CC_ASSERT(!dst->is_expired(), "upload target is a transient texture used past its epoch (expired)");
    CC_ASSERT(dst->usage().has(sg::texture_usage::copy_dst), "upload target texture must have "
                                                             "texture_usage::copy_dst");
    CC_ASSERT(dst->_upload_group != nullptr, "a copy_dst texture must carry an upload timeline");
}
} // namespace

void vulkan_upload_async_system::upload_texture(sg::raw_texture_handle const& texture,
                                                cc::pinned_data<byte const> data,
                                                sg::subresource_index const& subresource,
                                                sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "async upload target texture is null");
    auto const dst = std::dynamic_pointer_cast<vulkan_texture const>(texture);
    validate_texture_target(dst);
    if (data.empty())
        return;

    vulkan_async_upload_job job;
    job.texture_target = dst;
    job.is_texture = true;
    job.subresource = subresource;
    job.region = region;
    job.row_bytes = region_row_bytes(dst->description().format, region);
    job.src = cc::move(data);
    job.family = u64(reinterpret_cast<uintptr_t>(dst.get()));
    job.sequence = _next_sequence.fetch_add(1, cc::memory_order_relaxed);
    job.completion = {.group = dst->_upload_group, .value = dst->_upload_group->reserve()};

    dst->_pending_async_upload_value.store(job.completion.value, cc::memory_order_release);
    job.wait_token = sg::submission_token(dst->_last_used_submission_token.load(cc::memory_order_acquire));

    _actor->enqueue_message(cc::move(job));
}

sg::stream_upload_handle vulkan_upload_async_system::stream_texture(sg::raw_texture_handle const& texture,
                                                                    cc::pinned_data<byte const> data,
                                                                    sg::subresource_index const& subresource,
                                                                    sg::texture_region const& region)
{
    return stream_source_texture(texture, sg::make_pinned_stream_source(cc::move(data)), subresource, region);
}

sg::stream_upload_handle vulkan_upload_async_system::stream_source_texture(sg::raw_texture_handle const& texture,
                                                                           std::unique_ptr<sg::stream_source> source,
                                                                           sg::subresource_index const& subresource,
                                                                           sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "streaming upload target texture is null");
    CC_ASSERT(source != nullptr, "a streaming upload needs a source");
    auto const dst = std::dynamic_pointer_cast<vulkan_texture const>(texture);
    validate_texture_target(dst);

    auto control = std::make_shared<sg::impl::stream_control>();
    control->completion = cc::make_async_manual<cc::unit>();
    if (auto const hint = source->total_size_hint(); hint >= 0)
        control->total_hint.store(hint, std::memory_order_relaxed);

    vulkan_async_upload_job job;
    job.texture_target = dst;
    job.is_texture = true;
    job.subresource = subresource;
    job.region = region;
    job.row_bytes = region_row_bytes(dst->description().format, region);
    job.source = cc::move(source);
    job.stream = control;
    job.family = u64(reinterpret_cast<uintptr_t>(dst.get()));
    job.sequence = _next_sequence.fetch_add(1, cc::memory_order_relaxed);
    job.completion = {.group = dst->_upload_group, .value = dst->_upload_group->reserve()};

    dst->_pending_stream_copy_value.store(job.completion.value, cc::memory_order_release);
    job.wait_token = sg::submission_token(dst->_last_used_submission_token.load(cc::memory_order_acquire));

    control->on_promote = [dst, value = job.completion.value]
    {
        u64 previous = dst->_pending_async_upload_value.load(cc::memory_order_relaxed);
        while (previous < value
               && !dst->_pending_async_upload_value.compare_exchange_weak(previous, value, cc::memory_order_acq_rel,
                                                                          cc::memory_order_relaxed))
        {
        }
    };

    _actor->enqueue_message(cc::move(job));
    return sg::stream_upload_handle(cc::move(control));
}

void vulkan_upload_async_system::shutdown()
{
    if (_ctx == nullptr)
        return;

    // Detached before the actor dies, so a source's late wake finds nothing rather than a dangling actor.
    if (_waker != nullptr)
        _waker->detach();

    if (_actor)
    {
        _actor->shutdown(); // drains every queued transfer first
        _actor = {};
    }

    for (int i = 0; i < k_window_count; ++i)
        wait_for_window(i);

    // Every transfer still pending or awaiting settlement is cancelled here.
    // A manual completion node nobody pushes parks its dependents forever, which is why this runs on the teardown
    // path too rather than only where a transfer ends naturally.
    settle_finished();
    for (auto& entry : _awaiting)
    {
        if (entry.stream != nullptr && entry.stream->completion != nullptr && !entry.stream->completion->is_ready())
            entry.stream->completion->push_error(cc::async_error::make_cancelled());
    }
    _awaiting.clear();
    for (auto& job : _pending)
    {
        signal_on_queue(job.completion);
        settle_now(job, /*delivered =*/false);
    }
    _pending.clear();

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
    _waker = {};
    _ctx = nullptr;
}
} // namespace sg::backend::vulkan
