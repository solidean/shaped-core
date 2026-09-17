#include "metal_transfer.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>
#include <shaped-graphics/backends/metal/metal_stream.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>

#include <thread>

namespace sg::backend::metal
{
namespace
{
/// The resource options an off-frame staging allocation is made with.
/// Shared storage, because the CPU writes or reads it; untracked, for the same reason every other resource here is.
constexpr MTL::ResourceOptions k_transfer_staging_options
    = MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked;
} // namespace

void metal_transfer_system::create(metal_context& ctx)
{
    CC_ASSERT(_queue == nullptr, "the transfer system is created once");
    _ctx = &ctx;

    auto const scope = autorelease_scope();

    NS::Error* error = nullptr;
    auto* const descriptor = MTL4::CommandQueueDescriptor::alloc()->init();
    descriptor->setLabel(ns_string("sg transfer queue"));
    _queue = ctx.device()->newMTL4CommandQueue(descriptor, &error);
    descriptor->release();
    CC_ASSERT(_queue != nullptr, "the metal device refused a transfer queue");

    auto* const stream_descriptor = MTL4::CommandQueueDescriptor::alloc()->init();
    stream_descriptor->setLabel(ns_string("sg stream queue"));
    _stream_queue = ctx.device()->newMTL4CommandQueue(stream_descriptor, &error);
    stream_descriptor->release();
    CC_ASSERT(_stream_queue != nullptr, "the metal device refused a streaming queue");

    _timeline = ctx.device()->newSharedEvent();
    CC_ASSERT(_timeline != nullptr, "the metal device refused a transfer timeline");

    // The transfer queue's work touches the same resources the frame's does, so it needs the same residency set.
    ctx.residency().attach_to(_queue);
    ctx.residency().attach_to(_stream_queue);
}

metal_transfer_system::claimed metal_transfer_system::claim_value(void const* resource)
{
    return _state.lock(
        [&](state& s)
        {
            auto& slot = s.pending_by_resource[resource];
            auto const previous = slot;
            slot = s.next_value++;
            return claimed{.value = slot, .previous = previous};
        });
}

void metal_transfer_system::forget_value(void const* resource, u64 value)
{
    // Only the newest transfer clears the entry, so a resource with two in flight keeps the higher value until both are
    // done — and the map stays empty of resources that have none, which is what makes keying it on an address safe.
    _state.lock(
        [&](state& s)
        {
            auto const* const found = s.pending_by_resource.get_ptr(resource);
            if (found != nullptr && *found == value)
                (void)s.pending_by_resource.erase(resource);
        });
}

void metal_transfer_system::wait_for_queues(void const* resource, submission_stamp const& stamp, u64 previous_transfer)
{
    // A transfer reads or writes bytes the direct queue may still be producing, and the two queues share no timeline of
    // their own — so the copy defers behind the last command list that named this resource.
    // Zero means no list ever did, and the submission event would never reach it.
    if (auto const wait = stamp.get(); wait > 0)
        _queue->wait(_ctx->epochs().submission_timeline(), wait);

    // And behind this resource's own previous transfer.
    // Two commits on one queue are ordered, but the copies inside them are not — an upload and the download that reads
    // it back have to be told, or the download's copy overlaps the upload's.
    if (previous_transfer > 0)
        _queue->wait(_timeline, previous_transfer);

    // And behind any streaming transfer still filling this resource.
    //
    // **Metal orders the async tier against streams, where dx12 does not** — see libs/graphics/shaped-graphics/docs/TODO.md, which records the gap
    // on the backend that has it.
    // The per-resource streaming timeline is what makes it cheap here: an async transfer waits on the same value a
    // command list would, and `promote_to_async` is then purely the statement of intent it is documented to be.
    if (auto const wait = pending_stream_wait(resource); wait.event != nullptr)
        _queue->wait(wait.event, wait.value);
}

void metal_transfer_system::commit(MTL4::CommandBuffer* command_buffer,
                                   MTL4::CommandAllocator* allocator,
                                   MTL::Buffer* staging,
                                   void const* resource,
                                   u64 value,
                                   cc::unique_function<void()> on_complete)
{
    auto* const pending = &_pending;
    auto* const ctx = _ctx;
    auto* const self = this;
    auto sink = _ctx->feedback_sink();
    auto finish = std::make_shared<cc::unique_function<void()>>(cc::move(on_complete));

    auto* const options = MTL4::CommitOptions::alloc()->init();
    options->addFeedbackHandler(^void(MTL4::CommitFeedback*) {
      (*finish)();

      ctx->residency().remove(staging);
      staging->release();
      command_buffer->release();
      self->forget_value(resource, value);

      // Last, and after `on_complete` has dropped the resource handle it held: a waiter released by this counter must
      // find every lifetime this transfer extended already given back.
      //
      // Reaching zero is what `are_transfers_drained` reports, so the completion machinery has to be told.
      // Routed through the sink because this runs on Apple's queue, possibly after shutdown — and only with threads,
      // since the singlethreaded pump polls `settle_due_completions` itself.
      if (pending->fetch_sub(1, std::memory_order_acq_rel) == 1)
      {
#if CC_HAS_THREADS
          sink->notify_drained();
#endif
      }
    });

    MTL4::CommandBuffer const* const buffers[] = {command_buffer};
    _queue->commit(buffers, 1, options);
    options->release();

    _queue->signalEvent(_timeline, value);
    _ctx->epochs().retire_allocator_with_epoch(allocator);
}

u64 metal_transfer_system::pending_value_for(sg::raw_buffer const& buffer) const
{
    return _state.lock(
        [&](state& s) -> u64
        {
            auto const* const found = s.pending_by_resource.get_ptr(static_cast<void const*>(&buffer));
            return found != nullptr ? *found : u64(0);
        });
}

u64 metal_transfer_system::pending_value_for(sg::raw_texture const& texture) const
{
    return _state.lock(
        [&](state& s) -> u64
        {
            auto const* const found = s.pending_by_resource.get_ptr(static_cast<void const*>(&texture));
            return found != nullptr ? *found : u64(0);
        });
}

void metal_transfer_system::upload_to_buffer(sg::raw_buffer_handle buffer,
                                             cc::pinned_data<byte const> const& data,
                                             isize offset_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "async upload target buffer is null");
    CC_ASSERT(!buffer->is_expired(), "async upload target is a transient resource used past its epoch");
    CC_ASSERT(buffer->usage().has(sg::buffer_usage::copy_dst), "async upload target lacks copy_dst usage");
    CC_ASSERT(offset_in_bytes >= 0, "async upload offset must be non-negative");
    CC_ASSERT(offset_in_bytes + data.size() <= buffer->size_in_bytes(), "async upload range exceeds the buffer");

    // ctx.upload forwards an empty upload here unchecked, and nothing is the right thing to do with it.
    if (data.empty())
        return;

    auto const scope = autorelease_scope();

    auto* const staging = _ctx->device()->newBuffer(NS::UInteger(data.size()), k_transfer_staging_options);
    CC_ASSERT(staging != nullptr, "the metal device refused an async upload staging buffer");
    staging->setLabel(ns_string("sg async upload"));
    _ctx->residency().add(staging);

    // The CPU copy happens now, on the calling thread, which is what lets the pin be released as soon as the GPU copy
    // is recorded rather than held until it runs.
    cc::memcpy(staging->contents(), data.data(), size_t(data.size()));

    // Held past the commit below: a value claimed here must be the value signalled next on this queue.
    auto const submit_guard = _submit.lock_scoped();
    auto const claim = claim_value(buffer.get());
    _pending.fetch_add(1, std::memory_order_acq_rel);

    auto* const allocator = _ctx->epochs().lease_allocator();
    auto* const command_buffer = _ctx->device()->newCommandBuffer();
    command_buffer->beginCommandBuffer(allocator);

    auto* const encoder = command_buffer->computeCommandEncoder();
    auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);
    wait_for_queues(buffer.get(), mtl_buffer.submission(), claim.previous);
    encoder->copyFromBuffer(staging, 0, mtl_buffer.buffer(), NS::UInteger(offset_in_bytes), NS::UInteger(data.size()));
    encoder->endEncoding();
    command_buffer->endCommandBuffer();

    // **The destination is held until the copy completes, and released explicitly rather than by the handler dying.**
    //
    // The buffer's own deferred deletion is gated on the epoch, not on this transfer — so a caller that drops its last
    // handle right after issuing an upload would otherwise have the MTLBuffer freed while the copy is still running.
    // The explicit reset matters because a handler's captures are destroyed when the handler is, which is after it
    // returns: a waiter released by the pending counter would otherwise still see the handle alive.
    //
    // The pin is NOT held: the bytes were copied into staging above, on this thread.
    auto held = std::make_shared<sg::raw_buffer_handle>(cc::move(buffer));
    commit(command_buffer, allocator, staging, held->get(), claim.value, [held] { held->reset(); });
}

sg::bytes_future metal_transfer_system::download_from_buffer(sg::raw_buffer_handle buffer,
                                                             isize offset_in_bytes,
                                                             isize size_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "async download source buffer is null");
    CC_ASSERT(!buffer->is_expired(), "async download source is a transient resource used past its epoch");
    CC_ASSERT(buffer->usage().has(sg::buffer_usage::copy_src), "async download source lacks copy_src usage");
    CC_ASSERT(offset_in_bytes >= 0 && size_in_bytes >= 0, "async download range must be non-negative");
    CC_ASSERT(offset_in_bytes + size_in_bytes <= buffer->size_in_bytes(), "async download range exceeds the buffer");

    if (size_in_bytes == 0)
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_ready_completion());

    auto const scope = autorelease_scope();

    auto* const staging = _ctx->device()->newBuffer(NS::UInteger(size_in_bytes), k_transfer_staging_options);
    CC_ASSERT(staging != nullptr, "the metal device refused an async download staging buffer");
    staging->setLabel(ns_string("sg async download"));
    _ctx->residency().add(staging);

    // Held past the commit below: a value claimed here must be the value signalled next on this queue.
    auto const submit_guard = _submit.lock_scoped();
    auto const claim = claim_value(buffer.get());
    _pending.fetch_add(1, std::memory_order_acq_rel);

    auto* const allocator = _ctx->epochs().lease_allocator();
    auto* const command_buffer = _ctx->device()->newCommandBuffer();
    command_buffer->beginCommandBuffer(allocator);

    auto* const encoder = command_buffer->computeCommandEncoder();
    auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);
    wait_for_queues(buffer.get(), mtl_buffer.submission(), claim.previous);
    encoder->copyFromBuffer(mtl_buffer.buffer(), NS::UInteger(offset_in_bytes), staging, 0, NS::UInteger(size_in_bytes));
    encoder->endEncoding();
    command_buffer->endCommandBuffer();

    auto destination = cc::pinned_data<byte>::create_uninitialized(size_in_bytes);
    auto completion = cc::make_async_manual<cc::unit>();

    // A WEAK pin, because dropping the future must cancel the copy.
    // The GPU work still runs — it is already committed — but the copy out is skipped and the completion never
    // settles, which is what "cancelled" means on this channel.
    auto weak_destination = std::weak_ptr<void const>(destination.pin());
    auto destination_span = destination.span();
    auto held_buffer = std::make_shared<sg::raw_buffer_handle>(cc::move(buffer));
    commit(command_buffer, allocator, staging, held_buffer->get(), claim.value,
           [held_buffer, weak_destination, destination_span, staging, size_in_bytes, completion]
           {
               if (auto const alive = weak_destination.lock(); alive != nullptr)
               {
                   cc::memcpy(destination_span.data(), staging->contents(), size_t(size_in_bytes));
                   completion->push_value(cc::unit{});
               }
               held_buffer->reset();
           });

    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(destination)), cc::move(completion));
}

void metal_transfer_system::upload_to_texture(sg::raw_texture_handle texture,
                                              cc::pinned_data<byte const> const& pixels,
                                              sg::subresource_index const& subresource,
                                              sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "async upload target texture is null");
    CC_ASSERT(!texture->is_expired(), "async upload target is a transient texture used past its epoch");
    CC_ASSERT(texture->usage().has(sg::texture_usage::copy_dst), "async upload target texture lacks copy_dst usage");

    // The region arrives resolved: sg has defaulted it to the whole subresource, bounds-checked it, and skipped it when
    // empty.
    auto const layout = staging_layout_of(texture->description().format, region);
    CC_ASSERT(pixels.size() == layout.size_in_bytes, "async upload pixel data size does not match the copy region");

    if (layout.size_in_bytes == 0)
        return;

    auto const scope = autorelease_scope();

    auto* const staging = _ctx->device()->newBuffer(NS::UInteger(layout.size_in_bytes), k_transfer_staging_options);
    CC_ASSERT(staging != nullptr, "the metal device refused an async texture upload staging buffer");
    staging->setLabel(ns_string("sg async texture upload"));
    _ctx->residency().add(staging);

    cc::memcpy(staging->contents(), pixels.data(), size_t(layout.size_in_bytes));

    // Held past the commit below: a value claimed here must be the value signalled next on this queue.
    auto const submit_guard = _submit.lock_scoped();
    auto const claim = claim_value(texture.get());
    _pending.fetch_add(1, std::memory_order_acq_rel);

    auto* const allocator = _ctx->epochs().lease_allocator();
    auto* const command_buffer = _ctx->device()->newCommandBuffer();
    command_buffer->beginCommandBuffer(allocator);

    auto* const encoder = command_buffer->computeCommandEncoder();
    auto const& mtl_texture = static_cast<metal_texture const&>(*texture);
    wait_for_queues(texture.get(), mtl_texture.submission(), claim.previous);
    encoder->copyFromBuffer(
        staging, 0, NS::UInteger(layout.bytes_per_row), NS::UInteger(layout.bytes_per_image),
        MTL::Size(NS::UInteger(region.size[0]), NS::UInteger(region.size[1]), NS::UInteger(region.size[2])),
        mtl_texture.texture(), NS::UInteger(subresource.array_layer), NS::UInteger(subresource.mip_level),
        MTL::Origin(NS::UInteger(region.offset[0]), NS::UInteger(region.offset[1]), NS::UInteger(region.offset[2])));
    encoder->endEncoding();
    command_buffer->endCommandBuffer();

    auto held = std::make_shared<sg::raw_texture_handle>(cc::move(texture));
    commit(command_buffer, allocator, staging, held->get(), claim.value, [held] { held->reset(); });
}

sg::bytes_future metal_transfer_system::download_from_texture(sg::raw_texture_handle texture,
                                                              sg::subresource_index const& subresource,
                                                              sg::texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "async download source texture is null");
    CC_ASSERT(!texture->is_expired(), "async download source is a transient texture used past its epoch");
    CC_ASSERT(texture->usage().has(sg::texture_usage::copy_src), "async download source texture lacks copy_src usage");

    auto const layout = staging_layout_of(texture->description().format, region);
    if (layout.size_in_bytes == 0)
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_ready_completion());

    auto const scope = autorelease_scope();

    auto* const staging = _ctx->device()->newBuffer(NS::UInteger(layout.size_in_bytes), k_transfer_staging_options);
    CC_ASSERT(staging != nullptr, "the metal device refused an async texture download staging buffer");
    staging->setLabel(ns_string("sg async texture download"));
    _ctx->residency().add(staging);

    // Held past the commit below: a value claimed here must be the value signalled next on this queue.
    auto const submit_guard = _submit.lock_scoped();
    auto const claim = claim_value(texture.get());
    _pending.fetch_add(1, std::memory_order_acq_rel);

    auto* const allocator = _ctx->epochs().lease_allocator();
    auto* const command_buffer = _ctx->device()->newCommandBuffer();
    command_buffer->beginCommandBuffer(allocator);

    auto* const encoder = command_buffer->computeCommandEncoder();
    auto const& mtl_texture = static_cast<metal_texture const&>(*texture);
    wait_for_queues(texture.get(), mtl_texture.submission(), claim.previous);
    encoder->copyFromTexture(
        mtl_texture.texture(), NS::UInteger(subresource.array_layer), NS::UInteger(subresource.mip_level),
        MTL::Origin(NS::UInteger(region.offset[0]), NS::UInteger(region.offset[1]), NS::UInteger(region.offset[2])),
        MTL::Size(NS::UInteger(region.size[0]), NS::UInteger(region.size[1]), NS::UInteger(region.size[2])), staging, 0,
        NS::UInteger(layout.bytes_per_row), NS::UInteger(layout.bytes_per_image));
    encoder->endEncoding();
    command_buffer->endCommandBuffer();

    auto destination = cc::pinned_data<byte>::create_uninitialized(layout.size_in_bytes);
    auto completion = cc::make_async_manual<cc::unit>();
    auto weak_destination = std::weak_ptr<void const>(destination.pin());
    auto destination_span = destination.span();
    auto const size_in_bytes = layout.size_in_bytes;

    auto held = std::make_shared<sg::raw_texture_handle>(cc::move(texture));
    commit(command_buffer, allocator, staging, held->get(), claim.value,
           [held, weak_destination, destination_span, staging, size_in_bytes, completion]
           {
               if (auto const alive = weak_destination.lock(); alive != nullptr)
               {
                   cc::memcpy(destination_span.data(), staging->contents(), size_t(size_in_bytes));
                   completion->push_value(cc::unit{});
               }
               held->reset();
           });

    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(destination)), cc::move(completion));
}

void metal_transfer_system::commit_stream_batch(MTL4::CommandBuffer* command_buffer,
                                                MTL4::CommandAllocator* allocator,
                                                cc::unique_function<void()> on_complete)
{
    _pending.fetch_add(1, std::memory_order_acq_rel);

    auto* const pending = &_pending;
    auto sink = _ctx->feedback_sink();
    auto finish = std::make_shared<cc::unique_function<void()>>(cc::move(on_complete));

    auto* const options = MTL4::CommitOptions::alloc()->init();
    options->addFeedbackHandler(^void(MTL4::CommitFeedback*) {
      (*finish)();
      if (pending->fetch_sub(1, std::memory_order_acq_rel) == 1)
      {
#if CC_HAS_THREADS
          sink->notify_drained();
#endif
      }
    });

    MTL4::CommandBuffer const* const buffers[] = {command_buffer};
    _stream_queue->commit(buffers, 1, options);
    options->release();

    // No value signalled here: a streaming batch is one slice of a transfer, and what a waiter waits for is the
    // transfer ending — which is the per-resource stream timeline rather than this one.
    _ctx->epochs().retire_allocator_with_epoch(allocator);
}

void metal_transfer_system::order_stream_copy(metal_stream_job const& job)
{
    if (job.direct_wait > 0)
        _stream_queue->wait(_ctx->epochs().submission_timeline(), job.direct_wait);
}

u64 metal_transfer_system::reserve_stream_value(void const* resource)
{
    return _state.lock(
        [&](state& s)
        {
            auto& timeline = s.stream_timelines[resource];
            if (timeline.event == nullptr)
            {
                timeline.event = _ctx->device()->newSharedEvent();
                CC_ASSERT(timeline.event != nullptr, "the metal device refused a streaming timeline");
            }
            return timeline.next_value++;
        });
}

void metal_transfer_system::signal_stream_value(void const* resource, u64 value)
{
    _state.lock(
        [&](state& s)
        {
            auto* const timeline = s.stream_timelines.get_ptr(resource);
            if (timeline == nullptr || timeline->event == nullptr)
                return;
            if (timeline->event->signaledValue() < value)
                timeline->event->setSignaledValue(value);
        });
}

metal_transfer_system::stream_wait metal_transfer_system::pending_stream_wait(void const* resource) const
{
    return _state.lock(
        [&](state& s) -> stream_wait
        {
            auto const* const timeline = s.stream_timelines.get_ptr(resource);
            if (timeline == nullptr || timeline->event == nullptr)
                return {};

            // `next_value` is one past the highest reserved, and a value already signalled needs no wait.
            auto const highest = timeline->next_value - 1;
            if (highest == 0 || timeline->event->signaledValue() >= highest)
                return {};
            return {.event = timeline->event, .value = highest};
        });
}

void metal_transfer_system::shutdown()
{
    if (_queue == nullptr)
        return;

    // Every outstanding transfer has to finish before the queue and the timeline go, and a handler still in flight
    // touches both.
    while (_pending.load(std::memory_order_acquire) > 0)
        std::this_thread::yield();

    _state.lock(
        [](state& s)
        {
            for (auto&& [resource, timeline] : s.stream_timelines)
                if (timeline.event != nullptr)
                    timeline.event->release();
            s.stream_timelines.clear();
        });

    _timeline->release();
    _timeline = nullptr;
    _stream_queue->release();
    _stream_queue = nullptr;
    _queue->release();
    _queue = nullptr;
    _ctx = nullptr;
}
} // namespace sg::backend::metal
