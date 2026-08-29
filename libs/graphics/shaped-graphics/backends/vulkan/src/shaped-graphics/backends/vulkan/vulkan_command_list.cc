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

    // A texture finalize can return barriers — reverting to the canonical layout when other lists are still open —
    // so they are recorded onto this list before it closes.
    for (auto const* texture : cmd->_touched_textures)
        for (auto const& sub : texture->finalize_slot(cmd->slot()))
            cmd->_pending_image_barriers.push_back(make_image_barrier(texture->_image, sub.range, sub.barrier));
    cmd->_touched_textures.clear();
    submit_barriers(cmd->_buffer, {}, cmd->_pending_image_barriers);
    cmd->_pending_image_barriers.clear();

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

            // Inside the lock, so the actor's queue order matches submission order — which is also the order the
            // readback ring handed out its space.
            _download_inline.enqueue_submitted(t, cmd->_pending_downloads);
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
    for (auto const* texture : cmd._touched_textures)
        texture->discard_slot(cmd.slot());
    cmd._touched_textures.clear();

    // The recorded readbacks never run, so their futures are cancelled rather than left unsettled.
    // A future nobody ever settles parks its dependents for the life of the process.
    _download_inline.discard_unsubmitted(cmd._pending_downloads);

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

void vulkan_command_list::track_texture_access(vulkan_texture const& texture,
                                               sg::subresource_range range,
                                               sg::pipeline_stage_flags stages,
                                               sg::access_flags access,
                                               sg::texture_layout layout)
{
    if (texture._image == VK_NULL_HANDLE)
        return;

    texture.declare_access(_slot, range, stages, access, layout);

    if (texture.mark_pending_barrier(_slot))
        _pending_barrier_textures.push_back(&texture);

    if (texture.mark_recorded(_slot))
        _touched_textures.push_back(&texture);
}

void vulkan_command_list::flush_barriers()
{
    for (auto const* buffer : _pending_barrier_buffers)
        if (auto const barrier = buffer->flush_access(_slot); barrier.needed)
            _pending_buffer_barriers.push_back(make_buffer_barrier(buffer->_buffer, barrier));

    // A texture flush is per subresource box, so one texture may contribute several barriers.
    for (auto const* texture : _pending_barrier_textures)
        for (auto const& sub : texture->flush_access(_slot))
            _pending_image_barriers.push_back(make_image_barrier(texture->_image, sub.range, sub.barrier));

    _pending_barrier_buffers.clear();
    _pending_barrier_textures.clear();
    submit_barriers(_buffer, _pending_buffer_barriers, _pending_image_barriers);
    _pending_buffer_barriers.clear();
    _pending_image_barriers.clear();
}

namespace
{
// The tightly-packed byte size of `region` in `format`, and the alignment its staging offset needs.
// Vulkan takes tightly-packed image data when bufferRowLength / bufferImageHeight are 0, so unlike D3D12 there is no
// row-pitch padding to compute — only the total, and the offset rule.
struct texture_staging_layout
{
    isize size_in_bytes = 0;
    isize alignment = 4;
};

[[nodiscard]] texture_staging_layout staging_layout_of(sg::pixel_format format, sg::texture_region const& region)
{
    int const block_extent = sg::format_block_extent(format);
    int const block_size = sg::format_block_size(format);

    // A block-compressed format stores whole blocks, so a partial block at an edge still costs a full one.
    isize const blocks_x = (region.size[0] + block_extent - 1) / block_extent;
    isize const blocks_y = (region.size[1] + block_extent - 1) / block_extent;

    // bufferOffset must be a multiple of 4 and of the texel block size.
    isize const alignment = block_size % 4 == 0 ? isize(block_size) : isize(block_size) * 4;
    return {.size_in_bytes = blocks_x * blocks_y * isize(region.size[2]) * isize(block_size), .alignment = alignment};
}

// The image subresource one sg subresource_index addresses.
[[nodiscard]] VkImageSubresourceLayers image_subresource_of(sg::subresource_index const& sub)
{
    return VkImageSubresourceLayers{
        .aspectMask = vk_aspect_mask_from(sg::subresource_range(sub)),
        .mipLevel = u32(sub.mip_level),
        .baseArrayLayer = u32(sub.array_layer),
        .layerCount = 1,
    };
}
} // namespace

void vulkan_command_list::upload_bytes_to_texture(sg::raw_texture_handle texture,
                                                  cc::span<byte const> pixels,
                                                  sg::subresource_index const& subresource,
                                                  sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "upload target texture is null");
    auto const dst = std::dynamic_pointer_cast<vulkan_texture const>(texture);
    CC_ASSERT(dst != nullptr, "texture is not a vulkan texture");
    CC_ASSERT(!dst->is_expired(), "upload target is a transient texture used past its epoch (expired)");
    CC_ASSERT(dst->usage().has(sg::texture_usage::copy_dst), "upload target texture must have "
                                                             "texture_usage::copy_dst");

    // The region arrives resolved: sg has defaulted it to the whole subresource, bounds-checked it, and skipped it
    // when empty.
    auto const layout = staging_layout_of(dst->description().format, region);
    CC_ASSERT(pixels.size() == layout.size_in_bytes, "pixel data size does not match the copy region");

    auto const staging = _ctx._upload_inline.reserve(layout.size_in_bytes, layout.alignment);
    cc::memcpy(staging.mapped, pixels.data(), size_t(layout.size_in_bytes));

    track_texture_access(*dst, sg::subresource_range(subresource), sg::pipeline_stage_flag::copy,
                         sg::access_flag::copy_write, sg::texture_layout::copy_dst);
    flush_barriers();

    auto const copy = VkBufferImageCopy{
        .bufferOffset = VkDeviceSize(staging.offset),
        // Zero means tightly packed to imageExtent, which is exactly what sg hands us.
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = image_subresource_of(subresource),
        .imageOffset = {region.offset[0], region.offset[1], region.offset[2]},
        .imageExtent = {u32(region.size[0]), u32(region.size[1]), u32(region.size[2])},
    };
    vkCmdCopyBufferToImage(_buffer, staging.buffer, dst->_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

sg::bytes_future vulkan_command_list::download_bytes_from_texture(sg::raw_texture_handle texture,
                                                                  sg::subresource_index const& subresource,
                                                                  sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "download source texture is null");
    auto const src = std::dynamic_pointer_cast<vulkan_texture const>(texture);
    CC_ASSERT(src != nullptr, "texture is not a vulkan texture");
    CC_ASSERT(!src->is_expired(), "download source is a transient texture used past its epoch (expired)");
    CC_ASSERT(src->usage().has(sg::texture_usage::copy_src), "download source texture must have "
                                                             "texture_usage::copy_src");

    auto const layout = staging_layout_of(src->description().format, region);
    auto dst = cc::pinned_data<byte>::create_uninitialized(layout.size_in_bytes);
    auto const dst_span = dst.span();
    auto completion = cc::make_async_manual<cc::unit>();
    auto gate = std::make_shared<sg::bytes_wait_gate>();

    auto const staging = _ctx._download_inline.reserve(layout.size_in_bytes, layout.alignment);

    track_texture_access(*src, sg::subresource_range(subresource), sg::pipeline_stage_flag::copy,
                         sg::access_flag::copy_read, sg::texture_layout::copy_src);
    flush_barriers();

    auto const copy = VkBufferImageCopy{
        .bufferOffset = VkDeviceSize(staging.offset),
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = image_subresource_of(subresource),
        .imageOffset = {region.offset[0], region.offset[1], region.offset[2]},
        .imageExtent = {u32(region.size[0]), u32(region.size[1]), u32(region.size[2])},
    };
    vkCmdCopyImageToBuffer(_buffer, src->_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);

    _ctx._download_inline.account_pending_copy(staging.epoch_copies);

    vulkan_download_copy_job job;
    auto const size = layout.size_in_bytes;
    job.deferred_cpu_copy
        = [source = staging.mapped, dst_span, size] { cc::memcpy(dst_span.data(), source, size_t(size)); };
    job.pin = std::weak_ptr<void const>(dst.pin());
    job.completion = completion;
    job.gate = gate;
    job.epoch_copies = staging.epoch_copies;
    _pending_downloads.push_back(cc::move(job));

    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(dst)), cc::move(completion), cc::move(gate));
}

void vulkan_command_list::upload_bytes_to_buffer(sg::raw_buffer_handle buffer,
                                                 cc::span<byte const> data,
                                                 isize offset_in_bytes)
{
    // The upload scope forwards straight to this seam without validating, so the contract is checked here.
    // Bounds are checked before the empty early-out, so an empty write at a bad offset is still a contract violation.
    CC_ASSERT(buffer != nullptr, "upload target buffer is null");
    auto const dst_handle = std::dynamic_pointer_cast<vulkan_buffer const>(buffer);
    CC_ASSERT(dst_handle != nullptr, "buffer is not a vulkan buffer");
    CC_ASSERT(!dst_handle->is_expired(), "upload target is a transient buffer used past its epoch (expired)");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + data.size() <= dst_handle->size_in_bytes(),
              "upload range is out of the buffer's bounds");
    if (data.empty())
        return;
    CC_ASSERT(dst_handle->usage().has(sg::buffer_usage::copy_dst), "upload target buffer must have "
                                                                   "buffer_usage::copy_dst");

    auto const& dst = *dst_handle;

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

sg::bytes_future vulkan_command_list::download_bytes_from_buffer(sg::raw_buffer_handle buffer,
                                                                 isize offset_in_bytes,
                                                                 isize size_in_bytes)
{
    // As with upload, the download scope forwards straight here, so this seam owns the contract checks.
    CC_ASSERT(buffer != nullptr, "download source buffer is null");
    auto const src_handle = std::dynamic_pointer_cast<vulkan_buffer const>(buffer);
    CC_ASSERT(src_handle != nullptr, "buffer is not a vulkan buffer");
    CC_ASSERT(!src_handle->is_expired(), "download source is a transient buffer used past its epoch (expired)");
    CC_ASSERT(size_in_bytes >= 0, "download size must be non-negative");
    CC_ASSERT(offset_in_bytes >= 0 && offset_in_bytes + size_in_bytes <= src_handle->size_in_bytes(),
              "download range is out of the buffer's bounds");

    // A zero-size read is a ready empty future by contract, and needs no staging.
    if (size_in_bytes == 0)
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_ready_completion());

    CC_ASSERT(src_handle->usage().has(sg::buffer_usage::copy_src), "download source buffer must have "
                                                                   "buffer_usage::copy_src");

    auto const& src = *src_handle;

    auto dst = cc::pinned_data<byte>::create_uninitialized(size_in_bytes);
    auto const dst_span = dst.span();
    auto completion = cc::make_async_manual<cc::unit>();
    auto gate = std::make_shared<sg::bytes_wait_gate>();

    // The GPU copy is recorded now; the memcpy out of the ring can only run once that copy has finished, which is
    // what the actor waits for.
    auto const staging = _ctx._download_inline.reserve(size_in_bytes);

    track_buffer_access(src, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read);
    flush_barriers();

    auto const region = VkBufferCopy{
        .srcOffset = VkDeviceSize(offset_in_bytes),
        .dstOffset = VkDeviceSize(staging.offset),
        .size = VkDeviceSize(size_in_bytes),
    };
    vkCmdCopyBuffer(_buffer, src._buffer, staging.buffer, 1, &region);

    _ctx._download_inline.account_pending_copy(staging.epoch_copies);

    vulkan_download_copy_job job;
    job.deferred_cpu_copy = [source = staging.mapped, dst_span, size_in_bytes]
    { cc::memcpy(dst_span.data(), source, size_t(size_in_bytes)); };
    job.pin = std::weak_ptr<void const>(dst.pin());
    job.completion = completion;
    job.gate = gate;
    job.epoch_copies = staging.epoch_copies;
    _pending_downloads.push_back(cc::move(job));

    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(dst)), cc::move(completion), cc::move(gate));
}

void vulkan_command_list::copy_buffer_region(sg::raw_buffer_handle src,
                                             sg::raw_buffer_handle dst,
                                             isize src_offset_in_bytes,
                                             isize dst_offset_in_bytes,
                                             isize size_in_bytes)
{
    CC_ASSERT(src != nullptr, "copy source buffer is null");
    CC_ASSERT(dst != nullptr, "copy dest buffer is null");
    auto const s = std::dynamic_pointer_cast<vulkan_buffer const>(src);
    auto const d = std::dynamic_pointer_cast<vulkan_buffer const>(dst);
    CC_ASSERT(s != nullptr && d != nullptr, "buffer is not a vulkan buffer");
    CC_ASSERT(!s->is_expired() && !d->is_expired(), "copy uses a transient buffer past its epoch (expired)");
    CC_ASSERT(size_in_bytes >= 0, "copy size must be non-negative");
    CC_ASSERT(src_offset_in_bytes >= 0 && src_offset_in_bytes + size_in_bytes <= s->size_in_bytes(),
              "copy source range is out of the buffer's bounds");
    CC_ASSERT(dst_offset_in_bytes >= 0 && dst_offset_in_bytes + size_in_bytes <= d->size_in_bytes(),
              "copy dest range is out of the buffer's bounds");
    if (size_in_bytes == 0)
        return;
    CC_ASSERT(s->usage().has(sg::buffer_usage::copy_src), "copy source buffer must have buffer_usage::copy_src");
    CC_ASSERT(d->usage().has(sg::buffer_usage::copy_dst), "copy dest buffer must have buffer_usage::copy_dst");

    bool const same_resource = s->_buffer == d->_buffer;
    if (same_resource)
        CC_ASSERT(dst_offset_in_bytes + size_in_bytes <= src_offset_in_bytes
                      || src_offset_in_bytes + size_in_bytes <= dst_offset_in_bytes,
                  "source and destination ranges overlap in a same-buffer copy");

    // A self-copy reads and writes one resource, so it declares a single combined access and produces one barrier.
    // Declaring it twice would have the tracker treat the read and the write as separate ops on the same resource.
    if (same_resource)
        track_buffer_access(*s, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read | sg::access_flag::copy_write);
    else
    {
        track_buffer_access(*s, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read);
        track_buffer_access(*d, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    }
    flush_barriers();

    auto const region = VkBufferCopy{
        .srcOffset = VkDeviceSize(src_offset_in_bytes),
        .dstOffset = VkDeviceSize(dst_offset_in_bytes),
        .size = VkDeviceSize(size_in_bytes),
    };
    vkCmdCopyBuffer(_buffer, s->_buffer, d->_buffer, 1, &region);
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
