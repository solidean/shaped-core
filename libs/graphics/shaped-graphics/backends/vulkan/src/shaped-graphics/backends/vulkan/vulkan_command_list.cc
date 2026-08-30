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
                                         VkCommandBuffer buffer,
                                         VkCommandBuffer pre_buffer)
  : sg::command_list(ctx, created_in), _ctx(ctx), _slot(slot), _pool(pool), _buffer(buffer), _pre_buffer(pre_buffer)
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
    // The pool and both its buffers are recycled as a unit.
    vulkan_command_pool const reused = _command_pools.lock(
        [](vulkan_command_pool_set& p) -> vulkan_command_pool
        {
            if (p.free.empty())
                return {};
            return p.free.pop_back();
        });

    VkCommandPool pool = reused.pool;
    VkCommandBuffer buffer = reused.buffer;
    VkCommandBuffer pre_buffer = reused.pre_buffer;
    if (pool != VK_NULL_HANDLE)
    {
        // Recycle: reset returns the pool's buffers to the initial state, ready to record into again.
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

        // Two: the list's own, plus the pre-list that carries initial layout transitions ahead of it.
        auto const alloc_info = VkCommandBufferAllocateInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 2,
        };
        VkCommandBuffer allocated[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        if (VkResult r = vkAllocateCommandBuffers(_device, &alloc_info, allocated); r != VK_SUCCESS)
        {
            vkDestroyCommandPool(_device, pool, nullptr);
            return vulkan_error(r, "vkAllocateCommandBuffers failed");
        }
        buffer = allocated[0];
        pre_buffer = allocated[1];
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
    return std::make_unique<vulkan_command_list>(*this, current_epoch(), _command_list_slots.acquire(), pool, buffer,
                                                 pre_buffer);
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
    // Resolve the recorded queries first: it records copies of its own, so it has to happen before the finalize
    // barriers below and before the buffer is closed.
    cmd->finalize_queries_before_close();

    // The forward half of async sync: this list waits for any async upload still pending on a buffer it touches, so
    // it sees bytes the transfer queue wrote.
    //
    // Gathered per *timeline* rather than merged into one value, because a completion value only means anything on
    // the group that issued it — see vulkan_completion_group.
    // Deduplicated by keeping the highest value per group, since one list may touch several buffers sharing none.
    cc::vector<VkSemaphore> async_waits;
    cc::vector<u64> async_wait_values;
    auto const add_async_wait = [&](vulkan_completion_group_handle const& group, u64 value)
    {
        if (group == nullptr || value == 0 || group->has_reached(value))
            return; // already satisfied, so not worth a wait entry
        for (isize i = 0; i < async_waits.size(); ++i)
            if (async_waits[i] == group->timeline)
            {
                if (value > async_wait_values[i])
                    async_wait_values[i] = value;
                return;
            }
        async_waits.push_back(group->timeline);
        async_wait_values.push_back(value);
    };

    for (auto const& buffer : cmd->_touched_buffers)
    {
        add_async_wait(buffer->_upload_group, buffer->_pending_async_upload_value.load(cc::memory_order_acquire));
        add_async_wait(buffer->_download_group, buffer->_pending_async_download_value.load(cc::memory_order_acquire));
    }

    // Textures take the same pair, and must: the async transfer path reads a texture's canonical layout and restores
    // it, so without these edges that read names a layout the image is only in by luck.
    for (auto const& texture : cmd->_touched_textures)
    {
        add_async_wait(texture->_upload_group, texture->_pending_async_upload_value.load(cc::memory_order_acquire));
        add_async_wait(texture->_download_group, texture->_pending_async_download_value.load(cc::memory_order_acquire));
    }

    for (auto const& buffer : cmd->_touched_buffers)
        buffer->finalize_slot(cmd->slot());

    // A texture finalize can return barriers — reverting to the canonical layout when other lists are still open —
    // so they are recorded onto this list before it closes.
    for (auto const& texture : cmd->_touched_textures)
        for (auto const& sub : texture->finalize_slot(cmd->slot()))
            cmd->_pending_image_barriers.push_back(
                make_image_barrier(texture->_image, sub.range, texture->description().format, sub.barrier));
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

            // Bring every texture this list is the first to *run* into its resting layout, ahead of this list's own
            // work.
            //
            // Claimed here rather than while recording, and inside this lock rather than outside it: submission order
            // is what this lock serializes, so the list that claims is the list that runs first.
            // Claiming at record time would let a list that recorded second submit first, and its barriers would name
            // an oldLayout the image is not in yet.
            cc::vector<VkImageMemoryBarrier2> initial_barriers;
            for (auto const& texture : cmd->_tentative_initial_transitions)
            {
                if (!texture->claim_initial_transition())
                    continue; // someone got there first, and their transition carries it

                // UNDEFINED as the source is the discard: there are no contents to keep, which is what makes this
                // cheap.
                auto const barrier = sg::access_barrier{
                    .needed = true,
                    .src_layout = sg::texture_layout::undefined,
                    .dst_layout = texture->resting_layout(),
                };
                auto vk_barrier = make_image_barrier(
                    texture->_image, sg::subresource_range::whole(subresource_extent_of(texture->description())),
                    texture->description().format, barrier);

                // The destination scope is widened past anything sg::pipeline_stage_flag can spell, on purpose.
                // A barrier's second synchronization scope covers everything later in *submission order*, which
                // includes the whole of cmd->_buffer since the pre-list is submitted ahead of it in the same submit.
                // That is what makes the transition complete before the list's own first use of the texture, without
                // knowing here what that use is.
                // Set directly rather than through the flag set: naming every stage would include the ray-tracing
                // ones, which are invalid on a device without the extension.
                vk_barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                vk_barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                initial_barriers.push_back(vk_barrier);
            }

            VkCommandBuffer submitted_buffers[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
            u32 submitted_count = 0;
            if (!initial_barriers.empty())
            {
                auto const pre_begin = VkCommandBufferBeginInfo{
                    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                };
                VkResult const pre = vkBeginCommandBuffer(cmd->_pre_buffer, &pre_begin);
                CC_ASSERT(pre == VK_SUCCESS, "vkBeginCommandBuffer (initial transitions) failed");
                submit_barriers(cmd->_pre_buffer, {}, initial_barriers);
                VkResult const pre_end = vkEndCommandBuffer(cmd->_pre_buffer);
                CC_ASSERT(pre_end == VK_SUCCESS, "vkEndCommandBuffer (initial transitions) failed");
                submitted_buffers[submitted_count++] = cmd->_pre_buffer;
            }
            submitted_buffers[submitted_count++] = cmd->_buffer;

            // The submission timeline always signals; a presenting list also signals its render-finished semaphore.
            // The value array needs one entry per signal semaphore even though a binary one ignores its value.
            VkSemaphore signal_semaphores[2] = {_submission_timeline, cmd->_present_signal};
            u64 const signal_values[2] = {u64(t), 0};
            u32 const signal_count = cmd->_present_signal != VK_NULL_HANDLE ? 2u : 1u;

            // Waits: the async-transfer timelines gathered above, plus — for a presenting list — the acquire
            // semaphore, which must be satisfied before any color is written.
            cc::vector<VkSemaphore> waits = async_waits;
            cc::vector<u64> wait_values = async_wait_values;
            cc::vector<VkPipelineStageFlags> wait_stages;
            for (isize i = 0; i < async_waits.size(); ++i)
                wait_stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            if (cmd->_present_wait != VK_NULL_HANDLE)
            {
                waits.push_back(cmd->_present_wait);
                wait_values.push_back(0); // binary, so its value is ignored
                wait_stages.push_back(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            }
            u32 const wait_count = u32(waits.size());

            auto const timeline_info = VkTimelineSemaphoreSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                .waitSemaphoreValueCount = wait_count,
                .pWaitSemaphoreValues = wait_count != 0 ? wait_values.data() : nullptr,
                .signalSemaphoreValueCount = signal_count,
                .pSignalSemaphoreValues = signal_values,
            };
            auto const submit = VkSubmitInfo{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = &timeline_info,
                .waitSemaphoreCount = wait_count,
                .pWaitSemaphores = wait_count != 0 ? waits.data() : nullptr,
                .pWaitDstStageMask = wait_count != 0 ? wait_stages.data() : nullptr,
                .commandBufferCount = submitted_count,
                .pCommandBuffers = submitted_buffers,
                .signalSemaphoreCount = signal_count,
                .pSignalSemaphores = signal_semaphores,
            };
            VkResult const sr
                = _queue_guard.lock([&](int&) { return vkQueueSubmit(_queue, 1, &submit, VK_NULL_HANDLE); });
            // Record device loss here but don't throw inside the lock; the throw happens after it releases.
            if (sr != VK_SUCCESS && !note_device_lost_if_lost(sr, "vkQueueSubmit"))
                CC_ASSERT(false, "vkQueueSubmit failed");

            // The reverse half of async sync: a later async transfer on any of these resources defers behind this
            // token, so it never overwrites bytes this list still reads — nor, for a texture, transitions away from
            // the layout this list's barriers name.
            // Stamped inside the lock, so the token a transfer captures is never one from a list submitted after it.
            auto const stamp_reverse = [t](cc::atomic<u64>& slot)
            {
                u64 const stamp = u64(t);
                u64 previous = slot.load(cc::memory_order_relaxed);
                while (previous < stamp
                       && !slot.compare_exchange_weak(previous, stamp, cc::memory_order_acq_rel, cc::memory_order_relaxed))
                {
                }
            };
            for (auto const& buffer : cmd->_touched_buffers)
                stamp_reverse(buffer->_last_used_submission_token);
            for (auto const& texture : cmd->_touched_textures)
                stamp_reverse(texture->_last_used_submission_token);

            // Inside the lock, so the actor's queue order matches submission order — which is also the order the
            // readback ring handed out its space.
            _download_inline.enqueue_submitted(t, cmd->_pending_downloads);
            return t;
        });

    // The submit above may have observed device loss, marked rather than thrown inside the lock.
    // Surface it now that the lock is released — the context is dead, so the post-submit bookkeeping is moot.
    if (is_device_lost())
        throw sg::device_lost_exception(device_loss_reason());

    // Cleared only now: the reverse stamp inside the lock above reads these, so they cannot be emptied earlier.
    cmd->_touched_buffers.clear();
    cmd->_touched_textures.clear();
    cmd->_tentative_initial_transitions.clear();

    // The pool is in flight until this epoch retires, so hand it to the current epoch.
    // Null the list's handles so its destructor cannot destroy the pool just handed off.
    _command_pools.lock([&](vulkan_command_pool_set& p)
                        { p.in_epoch.push_back({cmd->_pool, cmd->_buffer, cmd->_pre_buffer}); });
    cmd->_pool = VK_NULL_HANDLE;
    cmd->_buffer = VK_NULL_HANDLE;
    cmd->_pre_buffer = VK_NULL_HANDLE;
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
    cmd.release_queries_on_drop();

    for (auto const& buffer : cmd._touched_buffers)
        buffer->discard_slot(cmd.slot());
    cmd._touched_buffers.clear();
    for (auto const& texture : cmd._touched_textures)
        texture->discard_slot(cmd.slot());
    cmd._touched_textures.clear();
    cmd._tentative_initial_transitions.clear();

    // The recorded readbacks never run, so their futures are cancelled rather than left unsettled.
    // A future nobody ever settles parks its dependents for the life of the process.
    _download_inline.discard_unsubmitted(cmd._pending_downloads);

    // Never submitted, so the GPU never touched this pool — return it straight to the free set, where reset happens at reuse.
    // Null the handles so nothing double-frees them.
    _command_pools.lock([&](vulkan_command_pool_set& p) { p.free.push_back({cmd._pool, cmd._buffer, cmd._pre_buffer}); });
    cmd._pool = VK_NULL_HANDLE;
    cmd._buffer = VK_NULL_HANDLE;
    cmd._pre_buffer = VK_NULL_HANDLE;
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
        _touched_buffers.push_back(std::static_pointer_cast<vulkan_buffer const>(buffer.shared_from_this()));
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
    {
        _touched_textures.push_back(std::static_pointer_cast<vulkan_texture const>(texture.shared_from_this()));

        // Tentative: another list recording right now may reach submit first and claim it.
        // Filtered there.
        if (texture.needs_initial_transition())
            _tentative_initial_transitions.push_back(_touched_textures.back());
    }
}

void vulkan_command_list::flush_barriers()
{
    for (auto const* buffer : _pending_barrier_buffers)
        if (auto const barrier = buffer->flush_access(_slot); barrier.needed)
            _pending_buffer_barriers.push_back(make_buffer_barrier(buffer->_buffer, barrier));

    // A texture flush is per subresource box, so one texture may contribute several barriers.
    for (auto const* texture : _pending_barrier_textures)
        for (auto const& sub : texture->flush_access(_slot))
            _pending_image_barriers.push_back(
                make_image_barrier(texture->_image, sub.range, texture->description().format, sub.barrier));

    _pending_barrier_buffers.clear();
    _pending_barrier_textures.clear();

    // A barrier is illegal inside a dynamic-rendering instance, so an open one is closed around it and reopened.
    // Nothing here decides whether that is cheap: a frame that transitions its resources before the scope opens
    // never reaches this, and one that does not pays a tile flush on a tiler.
    bool const suspend = _in_render_pass && !(_pending_buffer_barriers.empty() && _pending_image_barriers.empty());
    if (suspend)
        vkCmdEndRendering(_buffer);

    submit_barriers(_buffer, _pending_buffer_barriers, _pending_image_barriers);
    _pending_buffer_barriers.clear();
    _pending_image_barriers.clear();

    if (suspend)
        reopen_rendering();
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
// The format is what turns its positional aspect index into a Vulkan aspect bit; see sg::format_aspect_at.
[[nodiscard]] VkImageSubresourceLayers image_subresource_of(sg::subresource_index const& sub, sg::pixel_format format)
{
    return VkImageSubresourceLayers{
        .aspectMask = vk_aspect_mask_from(sg::subresource_range(sub), format),
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
        .imageSubresource = image_subresource_of(subresource, dst->description().format),
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
        .imageSubresource = image_subresource_of(subresource, src->description().format),
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

// Now the device's own answer, since every build and dispatch seam is real.
// The question this asks is whether THIS command list can trace rays rather than whether the GPU could — which is why
// it reported false while the seams were stubs, and why it can stop doing so only now.
bool vulkan_command_list::raytracing_is_supported() const
{
    return _ctx.is_raytracing_supported();
}
} // namespace sg::backend::vulkan
