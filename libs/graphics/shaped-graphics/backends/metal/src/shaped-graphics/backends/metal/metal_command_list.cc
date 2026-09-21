#include "metal_command_list.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>
#include <shaped-graphics/backends/metal/metal_acceleration_structure.hh>
#include <shaped-graphics/backends/metal/metal_binding_group.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/backends/metal/metal_compute_pipeline.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>
#include <shaped-graphics/backends/metal/metal_format.hh>
#include <shaped-graphics/backends/metal/metal_raster_pipeline.hh>
#include <shaped-graphics/backends/metal/metal_raster_state.hh>
#include <shaped-graphics/backends/metal/metal_raytracing_pipeline.hh>
#include <shaped-graphics/backends/metal/metal_raytracing_shader_table.hh>
#include <shaped-graphics/backends/metal/metal_staging_ring.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>
#include <shaped-graphics/exceptions.hh> // sg::exception, thrown where a recording seam has no error channel

// Everything below the constructor is a seam the milestone order has not reached; see
// libs/graphics/shaped-graphics/docs/writing-a-backend.md.
//
// A CC_UNREACHABLE stub *satisfies* the CHECK_ASSERTS tests written against these contracts, so those tests pass while
// the seam is unimplemented and start failing the moment it is filled in.
// That is a known trap rather than a surprise: copy the reference backend's assert list when implementing one, rather
// than inferring it from what the tests currently accept.
#define SG_METAL_UNIMPLEMENTED(what) CC_UNREACHABLE(what " is not implemented in the metal backend yet")

namespace sg::backend::metal
{
namespace
{
[[nodiscard]] MTL::LoadAction load_action_of(sg::target_op op)
{
    switch (op)
    {
    case sg::target_op::preserve:
        return MTL::LoadActionLoad;
    case sg::target_op::clear:
        return MTL::LoadActionClear;
    case sg::target_op::discard:
        return MTL::LoadActionDontCare;
    }
    return MTL::LoadActionLoad;
}

/// What happens to an attachment at pass end.
///
/// `discard` means the contents are undefined afterwards, so there is nothing to store — which on a tiler is a real
/// bandwidth saving rather than bookkeeping.
[[nodiscard]] MTL::StoreAction store_action_of(sg::target_op op)
{
    return op == sg::target_op::discard ? MTL::StoreActionDontCare : MTL::StoreActionStore;
}
} // namespace

metal_command_list::metal_command_list(metal_context& ctx,
                                       sg::epoch created_in,
                                       MTL4::CommandAllocator* allocator,
                                       MTL4::CommandBuffer* buffer)
  : sg::command_list(ctx, created_in), _metal_context(ctx), _allocator(allocator), _buffer(buffer)
{
    CC_ASSERT(_allocator != nullptr && _buffer != nullptr, "a metal command list needs both an allocator and a buffer");
    _buffer->beginCommandBuffer(_allocator);
    _slot = ctx.slots().acquire();
}

metal_command_list::~metal_command_list()
{
    // Ownership normally leaves through release_ownership at submit or drop.
    // A list destroyed still holding it was never handed to the context, so nothing else will free these — release them
    // here rather than leak, and say so, the way the vulkan backend does.
    if (_buffer == nullptr && _allocator == nullptr)
        return; // submitted or dropped: the context took the buffer, the allocator and the argument table

    CC_LOG_WARNING("command list destroyed without submit or drop — releasing it. Submit or drop every list you open.");

    end_recording(false);

    // The same unwind drop does, and for the same reason: a slot handed back still carrying this list's state makes
    // the next list on it skip every finalize, and a download nothing will ever run has to be cancelled rather than
    // left waiting.
    abandon_recording();

    if (_argument_table != nullptr)
        _argument_table->release();
    if (_buffer != nullptr)
        _buffer->release();
    if (_allocator != nullptr)
        _allocator->release();

    // The slot too, which submit and drop release and this path used to leak.
    // A leaked slot is permanent: the allocator hands out a bounded set, so enough of them exhaust the pool, and every
    // later advance_epoch sees a list that is not there.
    _metal_context.slots().release(_slot);
}

MTL4::ComputeCommandEncoder* metal_command_list::compute_encoder()
{
    if (_encoder != nullptr)
        return _encoder;

    // Retained for the same reason the render encoder is: an autoreleased object outlives this call only by whichever
    // pool happens to be current, and a submit opens one of its own.
    _encoder = _buffer->computeCommandEncoder()->retain();

    // Every encoder opens by waiting on everything already committed to this queue, and closes by publishing its own
    // work — the two halves of MTL4's queue barrier pair, emitted unconditionally.
    //
    // **What orders two command buffers is the submission timeline, not this pair.**
    // A queue wait between the two commits leaves this barrier with no earlier work to find, and a pending async
    // transfer puts one there — see the wait in metal_context::submit_command_list, which is what the ordering
    // actually rests on, and tests/barrier/cross-list-ordering-test.cc, which fails without it.
    // The pair stays for the visibility half, so a write published by one encoder is readable by the next.
    _encoder->barrierAfterQueueStages(k_compute_encoder_stages, k_compute_encoder_stages, MTL4::VisibilityOptionDevice);
    return _encoder;
}

MTL4::ArgumentTable* metal_command_list::argument_table()
{
    if (_argument_table != nullptr)
        return _argument_table;

    auto* const descriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
    // One buffer slot per binding group sg budgets, plus the one it reserves for itself — the whole address space a
    // pipeline layout can name.
    // Metal allows 31, so the cap is sg's rather than the API's.
    descriptor->setMaxBufferBindCount(NS::UInteger(sg::max_binding_groups + 1));
    descriptor->setInitializeBindings(true);
    descriptor->setLabel(ns_string("sg command list"));

    NS::Error* error = nullptr;
    _argument_table = _metal_context.device()->newArgumentTable(descriptor, &error);
    descriptor->release();

    // Thrown, not returned: this is a recording seam — `bind_group`, `bind_pipeline` — with no error channel of its
    // own, and a frame that cannot get an argument table has nothing useful left to record.
    // docs/error-handling.md reserves exceptions for exactly that: a failure the immediate caller cannot help with.
    if (_argument_table == nullptr)
        throw sg::exception(describe_error(error, "the metal device refused an argument table"));

    return _argument_table;
}

void metal_command_list::end_encoder()
{
    if (_encoder == nullptr)
        return;
    _encoder->endEncoding();
    _encoder->release();
    _encoder = nullptr;
}

MTL::Buffer* metal_command_list::retain_download_staging(metal_staging_ring::reservation const& staging)
{
    // An overflow buffer goes back with the epoch, which the copy out does not wait for: it runs from the commit's
    // feedback handler, and the host may have seen the fence long before.
    // One retain of its own is what keeps the bytes there until the copy has read them.
    if (staging.owned == nullptr)
        return nullptr;
    return staging.owned->retain();
}

cc::shared_ptr<cc::atomic<int>> metal_command_list::account_download_staging(metal_staging_ring::reservation const& staging)
{
    // The same problem for a reservation inside the ring, where a retain has nothing to hold: the span is charged to
    // the open epoch's copy count instead, and the ring will not hand those bytes out again until it drops to zero.
    if (staging.owned != nullptr)
        return nullptr;
    return _metal_context.download_ring().account_pending_copy();
}

void metal_command_list::pending_download::settle_staging()
{
    if (staging_retained != nullptr)
    {
        staging_retained->release();
        staging_retained = nullptr;
    }

    if (ring_copy != nullptr)
    {
        ring_copy->fetch_sub(1, std::memory_order_acq_rel);
        ring_copy = nullptr;
    }
}

void metal_command_list::abandon_recording()
{
    // The list's work never runs, so every resource it declared against is left exactly as it was.
    for (auto const& touched : _touched_buffers)
        static_cast<metal_buffer const&>(*touched).access().lock([&](metal_resource_access& a) { a.discard(_slot); });
    for (auto const& touched : _touched_textures)
        static_cast<metal_texture const&>(*touched).access().lock([&](metal_resource_access& a) { a.discard(_slot); });
    for (auto const& declared : _touched_accels)
        declared.storage->access().lock([&](metal_resource_access& a) { a.discard(_slot); });

    // And nothing will ever run the copy outs, so every future they would have settled is cancelled instead —
    // sg::bytes_future documents that as what a dropped recording list means.
    for (auto& download : _pending_downloads)
    {
        download.completion->push_error(cc::async_error::make_cancelled());
        download.settle_staging();
    }
    _pending_downloads.clear();

    release_queries_on_drop();
}

void metal_command_list::declare_buffer(raw_buffer_handle const& buffer, pipeline_stage_flags stages, access_flags access)
{
    CC_ASSERT(buffer != nullptr, "cannot declare access on a null buffer");
    CC_ASSERT(!buffer->is_expired(), "a transient resource was used past its epoch");

    auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);

    auto const [newly_pending, newly_recorded] = mtl_buffer.access().lock(
        [&](metal_resource_access& a)
        {
            a.declare(_slot, stages, access);
            return cc::pair{a.mark_pending_barrier(_slot), a.mark_recorded(_slot)};
        });

    if (newly_recorded)
        _touched_buffers.push_back(buffer);
    if (newly_pending)
        _pending_buffers.push_back(buffer);

    // Any declared access means this list records GPU work a later list may have to wait for.
    //
    // Set here rather than where a barrier is emitted, which is the bug this replaces: a list whose ops need no
    // barrier at all — an upload into a buffer nothing has touched — published nothing, so the next list's wait found
    // no producer and read what was there before.
    _produced_queue_work = true;
}

void metal_command_list::declare_texture(raw_texture_handle const& texture, pipeline_stage_flags stages, access_flags access)
{
    CC_ASSERT(texture != nullptr, "cannot declare access on a null texture");
    CC_ASSERT(!texture->is_expired(), "a transient resource was used past its epoch");

    auto const& mtl_texture = static_cast<metal_texture const&>(*texture);

    auto const [newly_pending, newly_recorded] = mtl_texture.access().lock(
        [&](metal_resource_access& a)
        {
            a.declare(_slot, stages, access);
            return cc::pair{a.mark_pending_barrier(_slot), a.mark_recorded(_slot)};
        });

    if (newly_recorded)
        _touched_textures.push_back(texture);
    if (newly_pending)
        _pending_textures.push_back(texture);

    _produced_queue_work = true;
}

void metal_command_list::adopt_overflow_staging(metal_staging_ring::reservation const& staging)
{
    if (staging.owned == nullptr)
        return;

    // An overflow reservation brought its own buffer, which has to be resident like any other and freed with the epoch.
    _metal_context.residency().add(staging.owned);

    auto* const owned = staging.owned;
    auto& ctx = _metal_context;
    _metal_context.epochs().defer(
        [&ctx, owned]
        {
            ctx.residency().remove(owned);
            owned->release();
        });
}

void metal_command_list::declare_accel(std::shared_ptr<void const> owner,
                                       metal_accel_storage const& storage,
                                       pipeline_stage_flags stages,
                                       access_flags access)
{
    CC_ASSERT(owner != nullptr, "cannot declare access on a null acceleration structure");
    CC_ASSERT(storage.accel() != nullptr, "an acceleration structure was used after it expired");

    auto const [newly_pending, newly_recorded] = storage.access().lock(
        [&](metal_resource_access& a)
        {
            a.declare(_slot, stages, access);
            return cc::pair{a.mark_pending_barrier(_slot), a.mark_recorded(_slot)};
        });

    if (newly_recorded)
        _touched_accels.push_back({owner, &storage});
    if (newly_pending)
        _pending_accels.push_back({cc::move(owner), &storage});

    _produced_queue_work = true;
}

void metal_command_list::flush_barriers()
{
    if (_pending_buffers.empty() && _pending_textures.empty() && _pending_accels.empty())
        return;

    // One MTL4 barrier names stages rather than resources, so every resource's requirement for this op folds into a
    // single stage pair — which is also why the batch is collected first and emitted once.
    MTL::Stages after = 0;
    MTL::Stages before = 0;
    auto visibility = MTL4::VisibilityOptions(MTL4::VisibilityOptionNone);
    auto any = false;

    for (auto const& buffer : _pending_buffers)
    {
        auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);
        auto const barrier = mtl_buffer.access().lock([&](metal_resource_access& a) { return a.flush(_slot); });

        auto const translated = translate_barrier(barrier);
        if (!translated.needed)
            continue;

        any = true;
        after |= translated.after_stages;
        before |= translated.before_stages;
        visibility = MTL4::VisibilityOptions(visibility | translated.visibility);
    }

    for (auto const& texture : _pending_textures)
    {
        auto const& mtl_texture = static_cast<metal_texture const&>(*texture);
        auto const barrier = mtl_texture.access().lock([&](metal_resource_access& a) { return a.flush(_slot); });

        auto const translated = translate_barrier(barrier);
        if (!translated.needed)
            continue;

        any = true;
        after |= translated.after_stages;
        before |= translated.before_stages;
        visibility = MTL4::VisibilityOptions(visibility | translated.visibility);
    }

    for (auto const& declared : _pending_accels)
    {
        auto const barrier = declared.storage->access().lock([&](metal_resource_access& a) { return a.flush(_slot); });

        auto const translated = translate_barrier(barrier);
        if (!translated.needed)
            continue;

        any = true;
        after |= translated.after_stages;
        before |= translated.before_stages;
        visibility = MTL4::VisibilityOptions(visibility | translated.visibility);
    }

    _pending_buffers.clear();
    _pending_textures.clear();
    _pending_accels.clear();

    if (!any)
        return;

    // Clamped to what a compute encoder can name: `barrierAfterEncoderStages` refuses any other stage, and with
    // validation armed it aborts rather than warning.
    // Nothing is lost while every op recorded here is a copy or a dispatch — a raster dependency will need the
    // queue-scoped form or an encoder boundary, which is the raster milestone's problem.
    compute_encoder()->barrierAfterEncoderStages(clamp_to_compute_encoder(after), clamp_to_compute_encoder(before),
                                                 visibility);
}

void metal_command_list::end_recording(bool will_submit)
{
    if (!_is_recording)
        return;
    _is_recording = false;

    // The producer half of the queue barrier pair.
    //
    // **A consumer barrier alone synchronizes nothing.** `barrierAfterQueueStages` at the head of a list waits on
    // queue work, and `barrierAfterStages` here is what publishes this list's work to the lists committed after it —
    // without it there is nothing for that wait to find, and a write in one command buffer stays invisible to a read
    // in the next.
    // That is the shape a decay rule hides on dx12 and a submission dependency hides nowhere: Metal wants both halves
    // spelled out.
    if (_produced_queue_work)
        compute_encoder()->barrierAfterStages(k_compute_encoder_stages, k_compute_encoder_stages,
                                              MTL4::VisibilityOptionDevice);

    end_encoder();

    // After the last encoder, since a resolve has to follow everything it measures.
    if (will_submit)
        finalize_queries_before_close();

    _buffer->endCommandBuffer();
}

void metal_command_list::release_ownership()
{
    _buffer = nullptr;
    _allocator = nullptr;
}

void metal_command_list::transition_texture_layout(raw_texture_handle,
                                                   texture_layout,
                                                   cc::optional<subresource_range> const&)
{
    // Nothing to do, and that is the whole of it: a Metal texture has no layout to be in.
    //
    // dx12 and vulkan both emit a real barrier here, and the caller's `cmd.ensure_layout` exists for them.
    // Honouring it as a no-op rather than asserting is what lets portable code call it unconditionally, which is what
    // it is for.
}

void metal_command_list::upload_bytes_to_buffer(raw_buffer_handle buffer, cc::span<byte const> data, isize offset_in_bytes)
{
    // cmd.upload forwards straight to this seam with no checking at all, so the whole contract is the backend's.
    // Bounds are checked BEFORE the empty early-out, so an empty write at a bad offset is still a violation.
    CC_ASSERT(buffer != nullptr, "upload target buffer is null");
    CC_ASSERT(!buffer->is_expired(), "upload target is a transient resource used past its epoch");
    CC_ASSERT(buffer->usage().has(sg::buffer_usage::copy_dst), "upload target lacks copy_dst usage");
    CC_ASSERT(offset_in_bytes >= 0, "upload offset must be non-negative");
    CC_ASSERT(offset_in_bytes + isize(data.size()) <= buffer->size_in_bytes(), "upload range exceeds the buffer");

    if (data.empty())
        return;

    auto const staging = _metal_context.upload_ring().reserve(isize(data.size()));
    if (!staging.is_valid())
    {
        // The device refused the dedicated allocation this reservation needed.
        // An inline upload has no error channel of its own, so it goes on the deferred one and the copy is not
        // recorded — which is the only honest thing left.
        _metal_context.report_feedback_error(sg::device_error_kind::creation_failed, "the metal device refused inline "
                                                                                     "upload staging");
        return;
    }

    adopt_overflow_staging(staging);

    // The CPU write happens now, at record time, into memory the GPU reads when the copy runs.
    cc::memcpy(staging.bytes().data(), data.data(), size_t(data.size()));

    declare_buffer(buffer, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    flush_barriers();

    auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);
    compute_encoder()->copyFromBuffer(staging.buffer, NS::UInteger(staging.offset), mtl_buffer.buffer(),
                                      NS::UInteger(offset_in_bytes), NS::UInteger(data.size()));
}

void metal_command_list::upload_bytes_to_texture(raw_texture_handle texture,
                                                 cc::span<byte const> pixels,
                                                 subresource_index const& subresource,
                                                 texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "upload target texture is null");
    CC_ASSERT(!texture->is_expired(), "upload target is a transient texture used past its epoch");
    CC_ASSERT(texture->usage().has(sg::texture_usage::copy_dst), "upload target texture lacks copy_dst usage");

    // The region arrives resolved: sg has defaulted it to the whole subresource, bounds-checked it, and skipped it
    // when empty.
    auto const layout = staging_layout_of(texture->description().format, region);
    CC_ASSERT(pixels.size() == layout.size_in_bytes, "pixel data size does not match the copy region");

    auto const staging = _metal_context.upload_ring().reserve(layout.size_in_bytes);
    if (!staging.is_valid())
    {
        _metal_context.report_feedback_error(sg::device_error_kind::creation_failed, "the metal device refused inline "
                                                                                     "texture upload staging");
        return;
    }
    adopt_overflow_staging(staging);

    cc::memcpy(staging.bytes().data(), pixels.data(), size_t(layout.size_in_bytes));

    declare_texture(texture, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    flush_barriers();

    auto const& mtl_texture = static_cast<metal_texture const&>(*texture);
    compute_encoder()->copyFromBuffer(
        staging.buffer, NS::UInteger(staging.offset), NS::UInteger(layout.bytes_per_row),
        NS::UInteger(layout.bytes_per_image),
        MTL::Size(NS::UInteger(region.size[0]), NS::UInteger(region.size[1]), NS::UInteger(region.size[2])),
        mtl_texture.texture(), NS::UInteger(subresource.array_layer), NS::UInteger(subresource.mip_level),
        MTL::Origin(NS::UInteger(region.offset[0]), NS::UInteger(region.offset[1]), NS::UInteger(region.offset[2])));
}

sg::bytes_future metal_command_list::download_bytes_from_buffer(raw_buffer_handle buffer,
                                                                isize offset_in_bytes,
                                                                isize size_in_bytes)
{
    CC_ASSERT(buffer != nullptr, "download source buffer is null");
    CC_ASSERT(!buffer->is_expired(), "download source is a transient resource used past its epoch");
    CC_ASSERT(buffer->usage().has(sg::buffer_usage::copy_src), "download source lacks copy_src usage");
    CC_ASSERT(offset_in_bytes >= 0 && size_in_bytes >= 0, "download range must be non-negative");
    CC_ASSERT(offset_in_bytes + size_in_bytes <= buffer->size_in_bytes(), "download range exceeds the buffer");

    if (size_in_bytes == 0)
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_ready_completion());

    auto const staging = _metal_context.download_ring().reserve(size_in_bytes);
    if (!staging.is_valid())
    {
        _metal_context.report_feedback_error(sg::device_error_kind::creation_failed, "the metal device refused inline "
                                                                                     "download staging");
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_cancelled_completion());
    }

    adopt_overflow_staging(staging);

    declare_buffer(buffer, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read);
    flush_barriers();

    auto const& mtl_buffer = static_cast<metal_buffer const&>(*buffer);
    compute_encoder()->copyFromBuffer(mtl_buffer.buffer(), NS::UInteger(offset_in_bytes), staging.buffer,
                                      NS::UInteger(staging.offset), NS::UInteger(size_in_bytes));

    // The destination the caller will read, and the node that says it is filled.
    auto destination = cc::pinned_data<byte>::create_uninitialized(size_in_bytes);
    auto completion = cc::make_async_manual<cc::unit>();

    // The copy out of the ring runs from the commit's own feedback handler, which fires once the GPU has finished the
    // command buffer this copy was recorded into.
    //
    // **No readback actor here**, where the other two backends have one: MTL4CommitFeedback already calls us at the
    // one moment this needs, so a thread of its own would buy nothing.
    // `block_until_transfers_drained` is what waits on the outstanding ones, which is the hook sg provides for exactly
    // this — an epoch deferral would not do, since a drain waits without advancing and an open epoch's
    // payload never runs.
    //
    // **The copy-out holds the destination**, sharing its owner rather than borrowing a span into it.
    // A caller is free to drop the future before its epoch retires — nothing in sg says otherwise — and a bare span
    // would then be written into freed memory, which shows up as heap corruption somewhere else entirely.
    _pending_downloads.push_back({.copy_out =
                                      [staging, destination, size_in_bytes, completion]() mutable
                                  {
                                      cc::memcpy(destination.data(), staging.bytes().data(), size_t(size_in_bytes));
                                      completion->push_value(cc::unit{});
                                  },
                                  .completion = completion,
                                  .staging_retained = retain_download_staging(staging),
                                  .ring_copy = account_download_staging(staging)});

    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(destination)), cc::move(completion));
}

sg::bytes_future metal_command_list::download_bytes_from_texture(raw_texture_handle texture,
                                                                 subresource_index const& subresource,
                                                                 texture_region const& region)
{
    CC_ASSERT(texture != nullptr, "download source texture is null");
    CC_ASSERT(!texture->is_expired(), "download source is a transient texture used past its epoch");
    CC_ASSERT(texture->usage().has(sg::texture_usage::copy_src), "download source texture lacks copy_src usage");

    auto const layout = staging_layout_of(texture->description().format, region);
    if (layout.size_in_bytes == 0)
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_ready_completion());

    auto const staging = _metal_context.download_ring().reserve(layout.size_in_bytes);
    if (!staging.is_valid())
    {
        _metal_context.report_feedback_error(sg::device_error_kind::creation_failed, "the metal device refused inline "
                                                                                     "texture download staging");
        return sg::bytes_future(cc::pinned_data<byte const>(), sg::make_cancelled_completion());
    }
    adopt_overflow_staging(staging);

    declare_texture(texture, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read);
    flush_barriers();

    auto const& mtl_texture = static_cast<metal_texture const&>(*texture);
    compute_encoder()->copyFromTexture(
        mtl_texture.texture(), NS::UInteger(subresource.array_layer), NS::UInteger(subresource.mip_level),
        MTL::Origin(NS::UInteger(region.offset[0]), NS::UInteger(region.offset[1]), NS::UInteger(region.offset[2])),
        MTL::Size(NS::UInteger(region.size[0]), NS::UInteger(region.size[1]), NS::UInteger(region.size[2])),
        staging.buffer, NS::UInteger(staging.offset), NS::UInteger(layout.bytes_per_row),
        NS::UInteger(layout.bytes_per_image));

    auto destination = cc::pinned_data<byte>::create_uninitialized(layout.size_in_bytes);
    auto completion = cc::make_async_manual<cc::unit>();
    auto const size = layout.size_in_bytes;

    _pending_downloads.push_back({.copy_out =
                                      [staging, destination, size, completion]() mutable
                                  {
                                      cc::memcpy(destination.data(), staging.bytes().data(), size_t(size));
                                      completion->push_value(cc::unit{});
                                  },
                                  .completion = completion,
                                  .staging_retained = retain_download_staging(staging),
                                  .ring_copy = account_download_staging(staging)});

    return sg::bytes_future(cc::pinned_data<byte const>(cc::move(destination)), cc::move(completion));
}

void metal_command_list::copy_buffer_region(raw_buffer_handle src,
                                            raw_buffer_handle dst,
                                            isize src_offset_in_bytes,
                                            isize dst_offset_in_bytes,
                                            isize size_in_bytes)
{
    CC_ASSERT(src != nullptr && dst != nullptr, "a copy needs both buffers");
    CC_ASSERT(size_in_bytes >= 0, "copy size must be non-negative");
    CC_ASSERT(src_offset_in_bytes >= 0 && dst_offset_in_bytes >= 0, "copy offsets must be non-negative");
    CC_ASSERT(src_offset_in_bytes + size_in_bytes <= src->size_in_bytes(), "copy source range exceeds the buffer");
    CC_ASSERT(dst_offset_in_bytes + size_in_bytes <= dst->size_in_bytes(), "copy dest range exceeds the buffer");
    CC_ASSERT(src->usage().has(sg::buffer_usage::copy_src), "copy source lacks copy_src usage");
    CC_ASSERT(dst->usage().has(sg::buffer_usage::copy_dst), "copy dest lacks copy_dst usage");

    // Bounds are checked before this, so an empty copy at a bad offset is still a contract violation.
    if (size_in_bytes == 0)
        return;

    auto const& mtl_src_check = static_cast<metal_buffer const&>(*src);
    auto const& mtl_dst_check = static_cast<metal_buffer const&>(*dst);
    auto const same_resource = mtl_src_check.buffer() == mtl_dst_check.buffer();
    if (same_resource)
        CC_ASSERT(dst_offset_in_bytes + size_in_bytes <= src_offset_in_bytes
                      || src_offset_in_bytes + size_in_bytes <= dst_offset_in_bytes,
                  "source and destination ranges overlap in a same-buffer copy");

    // A self-copy reads and writes one resource, so it declares a single combined access and produces one barrier.
    //
    // **Declaring it twice is the trap**, and it is silent: the tracker then treats the read and the write as two ops
    // on one resource, so the read is recorded as already ordered against the write it actually precedes.
    // The next op's read of the written range then needs no barrier by the tracker's reckoning, and reads what was
    // there before the copy.
    // Nothing reports it — the copy is encoded, the data is simply stale.
    if (same_resource)
        declare_buffer(src, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read | sg::access_flag::copy_write);
    else
    {
        declare_buffer(src, sg::pipeline_stage_flag::copy, sg::access_flag::copy_read);
        declare_buffer(dst, sg::pipeline_stage_flag::copy, sg::access_flag::copy_write);
    }
    flush_barriers();

    auto const& mtl_src = static_cast<metal_buffer const&>(*src);
    auto const& mtl_dst = static_cast<metal_buffer const&>(*dst);

    compute_encoder()->copyFromBuffer(mtl_src.buffer(), NS::UInteger(src_offset_in_bytes), mtl_dst.buffer(),
                                      NS::UInteger(dst_offset_in_bytes), NS::UInteger(size_in_bytes));
}

void metal_command_list::compute_bind_pipeline(compute_pipeline const& pipeline)
{
    auto const& mtl_pipeline = static_cast<metal_compute_pipeline const&>(pipeline);
    _bound_compute = &mtl_pipeline;
    _bound_layout = static_cast<metal_pipeline_layout const*>(mtl_pipeline.layout().get());

    auto* const encoder = compute_encoder();
    encoder->setComputePipelineState(mtl_pipeline.state());
    encoder->setArgumentTable(argument_table());
}

void metal_command_list::bind_group_to_table(int group_index, binding_group const& group)
{
    CC_ASSERT(group_index >= 0 && group_index < sg::max_binding_groups, "group index is out of range");

    auto const& mtl_group = static_cast<metal_binding_group const&>(group);

    // A layout that pins a group index may only ever be bound there, which is what makes a shader compiled against it
    // read the table slot it expects.
    if (auto const pinned = mtl_group.layout().group_index(); pinned.has_value())
        CC_ASSERT(int(pinned.value()) == group_index, "this binding group's layout pins it to a different group index");

    // The group's schema must be the one the bound pipeline's layout declared at this slot.
    // Metal binds an address rather than a descriptor table, so a mismatch is not a bind-time error anywhere below —
    // the shader simply reads an argument buffer laid out to a different schema.
    // libs/graphics/shaped-graphics/docs/concepts/bindings.md says every backend carries this check.
    CC_ASSERT(_bound_layout != nullptr, "bind a pipeline before binding its groups");
    auto const& declared = _bound_layout->description().groups;
    CC_ASSERT(group_index < isize(declared.size()), "the bound pipeline layout declares no group at this slot");
    CC_ASSERTF(declared[group_index].get() == &mtl_group.layout(), "{}",
               sg::impl::describe_layout_mismatch(group_index, declared[group_index].get(), &mtl_group.layout()));

    // The group's argument buffer address goes into the table's buffer slot, which IS the MSL [[buffer(N)]] index.
    argument_table()->setAddress(mtl_group.argument_address(), NS::UInteger(group_index));

    // The resources are copied rather than the group held: a binding_group arrives by reference and has no handle to
    // take, and what a draw or dispatch needs is the access list rather than the group itself.
    auto& slot_buffers = _group_buffers[group_index];
    slot_buffers.clear();
    for (auto const& buffer : mtl_group.bound_buffers())
        slot_buffers.push_back(buffer);

    auto& slot_textures = _group_textures[group_index];
    slot_textures.clear();
    for (auto const& texture : mtl_group.bound_textures())
        slot_textures.push_back(texture);

    auto& slot_tlases = _group_tlases[group_index];
    slot_tlases.clear();
    for (auto const& bound : mtl_group.bound_tlases())
        slot_tlases.push_back(bound);
}

void metal_command_list::compute_bind_group(int group_index, binding_group const& group)
{
    bind_group_to_table(group_index, group);
}

void metal_command_list::compute_dispatch(int x, int y, int z)
{
    CC_ASSERT(_bound_compute != nullptr, "a dispatch needs a bound compute pipeline");
    CC_ASSERT(x >= 0 && y >= 0 && z >= 0, "dispatch dimensions must be non-negative");

    if (x == 0 || y == 0 || z == 0)
        return;

    // Everything the bound groups name is read by this dispatch, so it is declared now rather than at bind time: a
    // group bound and then rebound before any dispatch never ran, and should leave no barrier behind.
    declare_bound_groups(sg::pipeline_stage_flag::compute);

    auto const size = _bound_compute->workgroup_size();
    compute_encoder()->dispatchThreadgroups(MTL::Size(NS::UInteger(x), NS::UInteger(y), NS::UInteger(z)),
                                            MTL::Size(NS::UInteger(size.x), NS::UInteger(size.y), NS::UInteger(size.z)));
}

void metal_command_list::compute_set_inline_constants(cc::span<byte const>, cc::optional<isize>)
{
    SG_METAL_UNIMPLEMENTED("compute inline constants");
}

void metal_command_list::compute_declare_array_buffer_access(cc::string_view, cc::span<array_buffer_access const>)
{
    SG_METAL_UNIMPLEMENTED("declaring compute array buffer access");
}

void metal_command_list::compute_declare_array_texture_access(cc::string_view, cc::span<array_texture_access const>)
{
    SG_METAL_UNIMPLEMENTED("declaring compute array texture access");
}


void metal_command_list::raster_bind_vertex_buffers(int, cc::span<vertex_buffer_view const>)
{
    SG_METAL_UNIMPLEMENTED("binding vertex buffers");
}

void metal_command_list::raster_bind_index_buffer(index_buffer_view const&)
{
    SG_METAL_UNIMPLEMENTED("binding an index buffer");
}


void metal_command_list::raster_set_inline_constants(cc::span<byte const>, cc::optional<isize>)
{
    SG_METAL_UNIMPLEMENTED("raster inline constants");
}


void metal_command_list::raster_draw_indexed(draw_indexed_config const&)
{
    SG_METAL_UNIMPLEMENTED("an indexed draw");
}

void metal_command_list::declare_bound_groups(pipeline_stage_flags stages)
{
    for (auto const& slot_buffers : _group_buffers)
        for (auto const& buffer : slot_buffers)
            declare_buffer(buffer, stages, sg::access_flag::shader_read | sg::access_flag::shader_write);
    for (auto const& slot_textures : _group_textures)
        for (auto const& texture : slot_textures)
            declare_texture(texture, stages, sg::access_flag::shader_read | sg::access_flag::shader_write);

    // A bound acceleration structure is read and never written by the work that traces it, which is why this one
    // declare is narrower than the two above.
    for (auto const& slot_tlases : _group_tlases)
        for (auto const& bound : slot_tlases)
        {
            auto const& mtl_tlas = static_cast<metal_tlas const&>(*bound);
            declare_accel(bound, mtl_tlas.storage(), stages, sg::access_flag::accel_read);
        }
    flush_barriers();
}

void metal_command_list::raster_begin_rendering(rendering_info const& info)
{
    CC_ASSERT(_render_encoder == nullptr, "a rendering scope is already open");

    // Declare and flush BEFORE the render encoder opens.
    //
    // A barrier cannot be emitted inside a render pass here any more than it can on vulkan, and closing and reopening
    // the pass around one — which vulkan does — would need every load op forced to LOAD to keep the contents.
    // Doing it first is cheaper, and it is what a frame that transitions its targets up front already gets.
    for (auto const& target : info.color_targets)
        declare_texture(target.view.texture(), sg::pipeline_stage_flag::render_target, sg::access_flag::color_write);
    if (info.depth_stencil_target.has_value())
        declare_texture(info.depth_stencil_target.value().view.texture(), sg::pipeline_stage_flag::depth_stencil_target,
                        sg::access_flag::depth_write);
    flush_barriers();

    // The compute encoder has to close first: Metal allows one encoder open at a time.
    end_encoder();
    auto const scope = autorelease_scope();
    auto* const descriptor = MTL4::RenderPassDescriptor::alloc()->init();

    auto width = 0;
    auto height = 0;

    for (auto i = isize(0); i < info.color_targets.size(); ++i)
    {
        auto const& target = info.color_targets[i];
        auto const& mtl_texture = static_cast<metal_texture const&>(*target.view.texture());
        auto* const attachment = descriptor->colorAttachments()->object(NS::UInteger(i));
        attachment->setTexture(mtl_texture.texture());
        attachment->setLevel(NS::UInteger(target.view.range().mip_range.start));
        attachment->setSlice(NS::UInteger(target.view.range().array_range.start));
        attachment->setLoadAction(load_action_of(target.op));
        attachment->setStoreAction(store_action_of(target.op));
        attachment->setClearColor(MTL::ClearColor(target.clear_color[0], target.clear_color[1], target.clear_color[2],
                                                  target.clear_color[3]));

        width = target.view.width();
        height = target.view.height();
    }

    if (info.depth_stencil_target.has_value())
    {
        auto const& target = info.depth_stencil_target.value();
        auto const& mtl_texture = static_cast<metal_texture const&>(*target.view.texture());

        auto* const attachment = descriptor->depthAttachment();
        attachment->setTexture(mtl_texture.texture());
        attachment->setLevel(NS::UInteger(target.view.range().mip_range.start));
        attachment->setSlice(NS::UInteger(target.view.range().array_range.start));
        attachment->setLoadAction(load_action_of(target.op));
        attachment->setStoreAction(store_action_of(target.op));
        attachment->setClearDepth(target.clear_depth);

        // A combined format is two attachments in Metal's model, where sg names one target.
        // Without the second, a stencil format gets no clear and every stencil test reads whatever was there.
        auto const format = target.view.texture()->description().format;
        if (sg::has_stencil(format))
        {
            auto* const stencil = descriptor->stencilAttachment();
            stencil->setTexture(mtl_texture.texture());
            stencil->setLevel(NS::UInteger(target.view.range().mip_range.start));
            stencil->setSlice(NS::UInteger(target.view.range().array_range.start));
            stencil->setLoadAction(load_action_of(target.op));
            stencil->setStoreAction(store_action_of(target.op));
            stencil->setClearStencil(target.clear_stencil);
        }

        width = width != 0 ? width : target.view.width();
        height = height != 0 ? height : target.view.height();
    }
    // **Retained, because it outlives the pool this function opened.**
    // `renderCommandEncoder` hands back an autoreleased object, and the scope above drains when this returns — so an
    // unretained encoder is deallocated the moment `begin_rendering` exits, and `end_rendering` then messages freed
    // memory.
    // It cost a sanitizer run to see, because the freed object is usually still readable.
    _scope_depth_stencil_format = info.depth_stencil_target.has_value()
                                    ? info.depth_stencil_target.value().view.texture()->description().format
                                    : sg::pixel_format::undefined;

    _render_encoder = _buffer->renderCommandEncoder(descriptor)->retain();
    descriptor->release();
    CC_ASSERT(_render_encoder != nullptr, "metal refused a render command encoder");

    // Every encoder is ordered against the queue on open, the same as the compute one.
    _render_encoder->barrierAfterQueueStages(MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionDevice);

    auto const vp = info.viewport.has_value()
                      ? info.viewport.value()
                      : sg::viewport{.offset = tg::pos2f(0.0f, 0.0f), .size = tg::vec2f(float(width), float(height))};
    _render_encoder->setViewport(
        MTL::Viewport{vp.offset[0], vp.offset[1], vp.size[0], vp.size[1], vp.min_depth, vp.max_depth});

    auto const rect
        = info.scissor.has_value()
            ? MTL::ScissorRect{NS::UInteger(info.scissor.value().min[0]), NS::UInteger(info.scissor.value().min[1]),
                               NS::UInteger(info.scissor.value().max[0] - info.scissor.value().min[0]),
                               NS::UInteger(info.scissor.value().max[1] - info.scissor.value().min[1])}
            : MTL::ScissorRect{0, 0, NS::UInteger(width), NS::UInteger(height)};
    _render_encoder->setScissorRect(rect);

    _produced_queue_work = true;
}

void metal_command_list::raster_end_rendering()
{
    CC_ASSERT(_render_encoder != nullptr, "no rendering scope is open");

    // Publish this pass to whatever is committed after it — the producer half of the queue barrier pair.
    _render_encoder->barrierAfterStages(MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionDevice);
    _render_encoder->endEncoding();
    _render_encoder->release();
    _render_encoder = nullptr;
    _bound_raster = nullptr;
    _scope_depth_stencil_format = sg::pixel_format::undefined;
}

void metal_command_list::raster_bind_pipeline(raster_pipeline const& pipeline)
{
    CC_ASSERT(_render_encoder != nullptr, "binding a raster pipeline needs an open rendering scope");

    auto const& mtl_pipeline = static_cast<metal_raster_pipeline const&>(pipeline);

    // The one thing `depth_stencil_format` is carried for: MTL4 builds the pipeline without it, so a pipeline drawn
    // into a scope whose depth target is a different format is not an error anywhere below this.
    CC_ASSERT(mtl_pipeline.depth_stencil_format() == _scope_depth_stencil_format,
              "this pipeline's depth_stencil_format is not the rendering scope's depth-stencil target format");

    _bound_raster = &mtl_pipeline;
    _bound_layout = static_cast<metal_pipeline_layout const*>(mtl_pipeline.layout().get());

    _render_encoder->setRenderPipelineState(mtl_pipeline.state());
    if (mtl_pipeline.depth_stencil_state() != nullptr)
        _render_encoder->setDepthStencilState(mtl_pipeline.depth_stencil_state());

    // Cull, fill, winding and depth bias are encoder state here rather than pipeline state, so they are replayed on
    // every bind — which is what keeps them consistent with the pipeline a caller believes is bound.
    auto const& raster = mtl_pipeline.rasterization();
    _render_encoder->setCullMode(cull_mode_of(raster.cull));
    _render_encoder->setTriangleFillMode(fill_mode_of(raster.fill));
    _render_encoder->setFrontFacingWinding(winding_of(raster.front));
    _render_encoder->setDepthBias(raster.depth_bias, raster.depth_bias_slope, raster.depth_bias_clamp);
    _render_encoder->setDepthClipMode(raster.depth_clip_enabled ? MTL::DepthClipModeClip : MTL::DepthClipModeClamp);

    _render_encoder->setArgumentTable(argument_table(), MTL::RenderStageVertex | MTL::RenderStageFragment);
}

void metal_command_list::raster_bind_group(int group_index, binding_group const& group)
{
    CC_ASSERT(_render_encoder != nullptr, "binding a group needs an open rendering scope");
    bind_group_to_table(group_index, group);
}

void metal_command_list::raster_set_viewport(viewport const& vp)
{
    CC_ASSERT(_render_encoder != nullptr, "setting the viewport needs an open rendering scope");
    _render_encoder->setViewport(
        MTL::Viewport{vp.offset[0], vp.offset[1], vp.size[0], vp.size[1], vp.min_depth, vp.max_depth});
}

void metal_command_list::raster_set_scissor(tg::aabb2i const& rect)
{
    CC_ASSERT(_render_encoder != nullptr, "setting the scissor needs an open rendering scope");
    _render_encoder->setScissorRect(MTL::ScissorRect{NS::UInteger(rect.min[0]), NS::UInteger(rect.min[1]),
                                                     NS::UInteger(rect.max[0] - rect.min[0]),
                                                     NS::UInteger(rect.max[1] - rect.min[1])});
}

void metal_command_list::raster_set_stencil_reference(u32 reference)
{
    CC_ASSERT(_render_encoder != nullptr, "setting the stencil reference needs an open rendering scope");
    _render_encoder->setStencilReferenceValue(reference);
}

void metal_command_list::raster_set_blend_constants(tg::vec4f constants)
{
    CC_ASSERT(_render_encoder != nullptr, "setting the blend constants needs an open rendering scope");
    _render_encoder->setBlendColor(constants[0], constants[1], constants[2], constants[3]);
}

void metal_command_list::raster_draw(draw_config const& config)
{
    CC_ASSERT(_render_encoder != nullptr, "a draw needs an open rendering scope");
    CC_ASSERT(_bound_raster != nullptr, "a draw needs a bound raster pipeline");

    if (config.vertex_range.size == 0 || config.instance_range.size == 0)
        return;

    _render_encoder->drawPrimitives(primitive_type_of(_bound_raster->topology()),
                                    NS::UInteger(config.vertex_range.offset), NS::UInteger(config.vertex_range.size),
                                    NS::UInteger(config.instance_range.size), NS::UInteger(config.instance_range.offset));
}

void metal_command_list::raytracing_bind_pipeline(raytracing_pipeline const& pipeline)
{
    // Nothing is bound to an encoder here: which compute pipeline state runs is decided by the raygen shader the
    // dispatch names, and the table is what resolves it.
    // So this only records the pipeline a later dispatch_rays must have been built for.
    _bound_raytracing = static_cast<metal_raytracing_pipeline const*>(&pipeline);
    _bound_layout = static_cast<metal_pipeline_layout const*>(_bound_raytracing->layout().get());
}

void metal_command_list::raytracing_bind_group(int group_index, binding_group const& group)
{
    CC_ASSERT(_bound_raytracing != nullptr, "bind a raytracing pipeline before binding its groups");
    bind_group_to_table(group_index, group);
}

void metal_command_list::raytracing_dispatch_rays(raytracing_shader_table const& table,
                                                  raygen_index raygen,
                                                  int width,
                                                  int height,
                                                  int depth)
{
    CC_ASSERT(_bound_raytracing != nullptr, "no raytracing pipeline is bound");
    CC_ASSERT(width >= 1 && height >= 1 && depth >= 1, "each dispatch_rays dimension must be >= 1");

    auto const& mtl_table = static_cast<metal_raytracing_shader_table const&>(table);
    CC_ASSERT(mtl_table.pipeline().get() == static_cast<sg::raytracing_pipeline const*>(_bound_raytracing),
              "this shader table was built for a different raytracing pipeline than the one bound");

    auto const& binding = mtl_table.binding_for(raygen);

    // The same declare-then-flush rhythm a dispatch uses, at the raytracing stage — a bound TLAS surfaces as
    // accel_read through the group's own declare.
    declare_bound_groups(sg::pipeline_stage_flag::raytracing);

    auto* const encoder = compute_encoder();
    encoder->setComputePipelineState(binding.state);
    encoder->setArgumentTable(argument_table());

    // The four function tables reach the kernel as the reserved group's argument buffer, above every group a caller
    // may bind — see sg::reserved_binding_group.
    argument_table()->setAddress(MTL::GPUAddress(binding.arguments->gpuAddress()),
                                 NS::UInteger(sg::reserved_binding_group));

    // A raygen kernel is dispatched by thread count rather than by threadgroup: sg's width/height/depth is a ray grid,
    // and Metal takes the threadgroup shape separately.
    // sg carries none on this call, so it comes from the pipeline — clamped per axis to the grid, because Metal
    // rejects a threadgroup larger than the grid in any dimension rather than trimming it.
    auto const max_threads = isize(binding.state->maxTotalThreadsPerThreadgroup());
    auto const group_x = cc::min(isize(width), cc::max(isize(1), isize(binding.state->threadExecutionWidth())));
    auto const group_y = cc::min(isize(height), cc::max(isize(1), max_threads / group_x));
    auto const group_z = cc::min(isize(depth), cc::max(isize(1), max_threads / (group_x * group_y)));

    encoder->dispatchThreads(MTL::Size(NS::UInteger(width), NS::UInteger(height), NS::UInteger(depth)),
                             MTL::Size(NS::UInteger(group_x), NS::UInteger(group_y), NS::UInteger(group_z)));
}

bool metal_command_list::query_timestamps_supported() const
{
    return _metal_context.queries().supports_timestamps();
}

sg::gpu_timestamp metal_command_list::query_record_gpu_timestamp()
{
    auto& queries = _metal_context.queries();

    // Callable rather than fatal, which is the contract for an unsupported backend: the caller gets an invalid query
    // that never becomes ready, and code written against a backend that has timestamps still runs here.
    // A stub would abort instead — and in a release build, where CC_ASSERT is off, take the process with it.
    if (!queries.supports_timestamps())
        return {};

    auto const needs_fresh = _active_timestamp_lease < 0
                          || _leased_counter_heaps[_active_timestamp_lease]->next_slot
                                 >= _leased_counter_heaps[_active_timestamp_lease]->slot_count;
    if (needs_fresh)
    {
        auto lease = queries.acquire_heap();
        if (lease == nullptr)
            return {}; // the device refused a heap mid-recording; the caller sees an invalid query, as when unsupported
        _active_timestamp_lease = int(_leased_counter_heaps.size());
        _leased_counter_heaps.push_back(cc::move(lease));
    }

    auto& lease = *_leased_counter_heaps[_active_timestamp_lease];
    auto const slot = lease.next_slot++;

    // **A timestamp is written at command-buffer level, not into an encoder**, so the open one has to close first.
    // That is also what makes the value meaningful: it is written once everything recorded before it has completed,
    // which is what makes the difference between two of them the duration of the work between.
    end_encoder();
    _buffer->writeTimestampIntoHeap(lease.heap, NS::UInteger(slot));

    return sg::gpu_timestamp(std::shared_ptr<sg::data_future<u64> const>(lease.shared_future), isize(slot),
                             queries.timestamp_tick_to_seconds());
}

void metal_command_list::finalize_queries_before_close()
{
    // Heaps are leased on demand, so every leased heap holds at least one recorded timestamp.
    for (auto& lease : _leased_counter_heaps)
    {
        CC_ASSERT(lease->next_slot > 0, "leased a counter heap and recorded nothing into it");
        auto const size_in_bytes = isize(lease->next_slot) * isize(sizeof(u64));

        auto const staging = _metal_context.download_ring().reserve(size_in_bytes);
        if (!staging.is_valid())
        {
            _metal_context.report_feedback_error(sg::device_error_kind::creation_failed, "the metal device refused "
                                                                                         "timestamp readback staging");
            continue; // the handles keep their invalid future, which reads as never ready
        }
        adopt_overflow_staging(staging);

        // Straight into the staging ring, where vulkan needs a transient buffer in between: a resolve names a GPU
        // address rather than a bound resource, so the ring's own buffer is as good a destination as any.
        // No fences: the resolve is ordered by the command buffer it sits in, like every other call here.
        _buffer->resolveCounterHeap(
            lease->heap, NS::Range(0, NS::UInteger(lease->next_slot)),
            MTL4::BufferRange(staging.buffer->gpuAddress() + u64(staging.offset), u64(size_in_bytes)), nullptr, nullptr);

        auto destination = cc::pinned_data<byte>::create_uninitialized(size_in_bytes);
        auto completion = cc::make_async_manual<cc::unit>();

        _pending_downloads.push_back({.copy_out =
                                          [staging, destination, size_in_bytes, completion]() mutable
                                      {
                                          cc::memcpy(destination.data(), staging.bytes().data(), size_t(size_in_bytes));
                                          completion->push_value(cc::unit{});
                                      },
                                      .completion = completion,
                                      .staging_retained = retain_download_staging(staging),
                                      .ring_copy = account_download_staging(staging)});

        // Assigned in place, so the handles already handed out see it.
        *lease->shared_future = sg::data_future<u64>(
            sg::bytes_future(cc::pinned_data<byte const>(cc::move(destination)), cc::move(completion)));
    }

    // Back through the epoch rather than straight away: the resolve just recorded still names each heap, right up
    // until this epoch's work has finished.
    for (auto& lease : _leased_counter_heaps)
        _metal_context.epochs().defer([system = &_metal_context.queries(), held = cc::move(lease)]() mutable
                                      { system->release_heap(cc::move(held)); });
    _leased_counter_heaps.clear();
    _active_timestamp_lease = -1;
}

void metal_command_list::release_queries_on_drop()
{
    // Straight back rather than through the epoch: nothing was committed, so no pending command names them.
    for (auto& lease : _leased_counter_heaps)
        _metal_context.queries().release_heap(cc::move(lease));
    _leased_counter_heaps.clear();
    _active_timestamp_lease = -1;
}
} // namespace sg::backend::metal
