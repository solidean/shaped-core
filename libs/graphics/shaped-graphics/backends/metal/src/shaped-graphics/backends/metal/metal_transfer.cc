#include "metal_transfer.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

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

    _timeline = ctx.device()->newSharedEvent();
    CC_ASSERT(_timeline != nullptr, "the metal device refused a transfer timeline");

    // The transfer queue's work touches the same resources the frame's does, so it needs the same residency set.
    ctx.residency().attach_to(_queue);
}

metal_transfer_system::claimed metal_transfer_system::claim_value(sg::raw_buffer const& buffer)
{
    return _state.lock(
        [&](state& s)
        {
            auto& slot = s.pending_by_buffer[&buffer];
            auto const previous = slot;
            slot = s.next_value++;
            return claimed{.value = slot, .previous = previous};
        });
}

void metal_transfer_system::forget_value(sg::raw_buffer const& buffer, u64 value)
{
    // Only the newest transfer clears the entry, so a resource with two in flight keeps the higher value until both are
    // done — and the map stays empty of resources that have none, which is what makes keying it on an address safe.
    _state.lock(
        [&](state& s)
        {
            auto const* const found = s.pending_by_buffer.get_ptr(&buffer);
            if (found != nullptr && *found == value)
                (void)s.pending_by_buffer.erase(&buffer);
        });
}

void metal_transfer_system::wait_for_queues(metal_buffer const& buffer, u64 previous_transfer)
{
    // A transfer reads or writes bytes the direct queue may still be producing, and the two queues share no timeline of
    // their own — so the copy defers behind the last command list that named this buffer.
    // Zero means no list ever did, and the submission event would never reach it.
    if (auto const wait = buffer.last_used_submission(); wait > 0)
        _queue->wait(_ctx->epochs().submission_timeline(), wait);

    // And behind this buffer's own previous transfer.
    // Two commits on one queue are ordered, but the copies inside them are not — an upload and the download that reads
    // it back have to be told, or the download's copy overlaps the upload's.
    if (previous_transfer > 0)
        _queue->wait(_timeline, previous_transfer);
}

u64 metal_transfer_system::pending_value_for(sg::raw_buffer const& buffer) const
{
    return _state.lock(
        [&](state& s) -> u64
        {
            auto const* const found = s.pending_by_buffer.get_ptr(&buffer);
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

    auto const claim = claim_value(*buffer);

    _pending.fetch_add(1, std::memory_order_acq_rel);

    auto* const allocator = _ctx->epochs().lease_allocator();
    auto* const command_buffer = _ctx->device()->newCommandBuffer();
    command_buffer->beginCommandBuffer(allocator);

    auto* const encoder = command_buffer->computeCommandEncoder();
    auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);
    wait_for_queues(mtl_buffer, claim.previous);
    encoder->copyFromBuffer(staging, 0, mtl_buffer.buffer(), NS::UInteger(offset_in_bytes), NS::UInteger(data.size()));
    encoder->endEncoding();
    command_buffer->endCommandBuffer();

    // **The destination is held until the copy completes, and released explicitly rather than by the block dying.**
    //
    // The sg buffer's own deferred deletion is gated on the epoch, not on this transfer — so a caller that drops its
    // last handle right after issuing an upload would otherwise have the MTLBuffer freed while the transfer queue is
    // still copying into it.
    // The explicit reset matters because a block's captures are destroyed when the block is, which is after the
    // handler returns: a waiter released by the counter below would otherwise still see the handle alive.
    auto held_buffer = std::make_shared<sg::raw_buffer_handle>(cc::move(buffer));

    // The pin is NOT held: the bytes were copied into staging above, on this thread, so the caller's memory is free to
    // go the moment this returns.
    // That is the whole reason the copy is synchronous here.
    auto* const pending = &_pending;
    auto* const ctx = _ctx;
    auto* const self = this;
    auto const value = claim.value;

    auto* const options = MTL4::CommitOptions::alloc()->init();
    options->addFeedbackHandler(^void(MTL4::CommitFeedback*) {
      ctx->residency().remove(staging);
      staging->release();
      command_buffer->release();
      self->forget_value(**held_buffer, value);
      held_buffer->reset();
      pending->fetch_sub(1, std::memory_order_acq_rel);
    });

    MTL4::CommandBuffer const* const buffers[] = {command_buffer};
    _queue->commit(buffers, 1, options);
    options->release();

    _queue->signalEvent(_timeline, value);
    _ctx->epochs().retire_allocator_with_epoch(allocator);
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

    auto const claim = claim_value(*buffer);

    _pending.fetch_add(1, std::memory_order_acq_rel);

    auto* const allocator = _ctx->epochs().lease_allocator();
    auto* const command_buffer = _ctx->device()->newCommandBuffer();
    command_buffer->beginCommandBuffer(allocator);

    auto* const encoder = command_buffer->computeCommandEncoder();
    auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);
    wait_for_queues(mtl_buffer, claim.previous);
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

    auto* const pending = &_pending;
    auto* const ctx = _ctx;
    auto* const self = this;
    auto const value = claim.value;
    auto held_buffer = std::make_shared<sg::raw_buffer_handle>(cc::move(buffer));

    auto* const options = MTL4::CommitOptions::alloc()->init();
    options->addFeedbackHandler(^void(MTL4::CommitFeedback*) {
      if (auto const alive = weak_destination.lock(); alive != nullptr)
      {
          cc::memcpy(destination_span.data(), staging->contents(), size_t(size_in_bytes));
          completion->push_value(cc::unit{});
      }

      ctx->residency().remove(staging);
      staging->release();
      command_buffer->release();
      self->forget_value(**held_buffer, value);
      held_buffer->reset();
      pending->fetch_sub(1, std::memory_order_acq_rel);
    });

    MTL4::CommandBuffer const* const buffers[] = {command_buffer};
    _queue->commit(buffers, 1, options);
    options->release();

    _queue->signalEvent(_timeline, value);
    _ctx->epochs().retire_allocator_with_epoch(allocator);

    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(destination)), cc::move(completion));
}

void metal_transfer_system::shutdown()
{
    if (_queue == nullptr)
        return;

    // Every outstanding transfer has to finish before the queue and the timeline go, and a handler still in flight
    // touches both.
    while (_pending.load(std::memory_order_acquire) > 0)
        std::this_thread::yield();

    _timeline->release();
    _timeline = nullptr;
    _queue->release();
    _queue = nullptr;
    _ctx = nullptr;
}
} // namespace sg::backend::metal
