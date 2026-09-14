#include "metal_context.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/record/log.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/exceptions.hh>

#include <thread>

// Seams the milestone order has not reached; see libs/graphics/shaped-graphics/docs/writing-a-backend.md.
#define SG_METAL_UNIMPLEMENTED(what) CC_UNREACHABLE(what " is not implemented in the metal backend yet")

namespace sg::backend::metal
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "sg.metal");

metal_context::metal_context(MTL::Device* device,
                             MTL4::CommandQueue* queue,
                             MTL4::Compiler* compiler,
                             MTL::SharedEvent* epoch_event,
                             MTL::SharedEvent* submission_event)
  : sg::context(sg::backend_kind::metal, sg::thread_model::multi_threaded, k_accepted_shader_formats),
    _device(device),
    _queue(queue),
    _compiler(compiler),
    _epochs(device, queue, epoch_event, submission_event)
{
    CC_ASSERT(_device != nullptr && _queue != nullptr, "a metal context needs a device and a queue");
    CC_ASSERT(_compiler != nullptr, "a metal context needs a compiler");
    _feedback = std::make_shared<metal_feedback_sink>(*this);
}

void metal_context::create_staging_rings(isize upload_bytes, isize download_bytes)
{
    // Before the rings, so their own buffers can declare themselves resident as they are made.
    _residency.create(_device, _queue);
    _transfers.create(*this);

    _upload_ring.lock([&](metal_staging_ring& r) { r.create(_device, upload_bytes, "sg inline upload ring"); });
    _download_ring.lock([&](metal_staging_ring& r) { r.create(_device, download_bytes, "sg inline download ring"); });

    _upload_ring.lock([&](metal_staging_ring& r) { _residency.add(r.buffer()); });
    _download_ring.lock([&](metal_staging_ring& r) { _residency.add(r.buffer()); });
}

void metal_context::report_feedback_error(sg::device_error_kind kind, cc::string_view message)
{
    if (kind == sg::device_error_kind::device_lost)
        mark_device_lost(cc::string(message));
    else
        report_device_error({.kind = kind, .message = cc::string(message)});
}

metal_context::~metal_context()
{
    shutdown_no_throw();
}

bool metal_context::supports(sg::feature f) const
{
    switch (f)
    {
    case sg::feature::raytracing:
        // The device has it; the backend does not yet, and reporting the device's answer would turn a clean skip into
        // a crash at the first build call.
        return false;
    case sg::feature::headless_present:
        // Always: the chain is emulated with ordinary render targets, so there is no surface extension to be missing.
        return true;
    case sg::feature::timestamp_query:
        return false;
    case sg::feature::geometry_shader:
    case sg::feature::tessellation_shader:
        // Metal has never had either stage; a caller asking gets a permanent answer rather than a temporary one.
        return false;
    }
    return false;
}

void metal_context::advance_epoch()
{
    // Auto-expire the closing epoch's transient resources before the epoch is packaged.
    //
    // The transient bump heap resets its head every epoch, so the next epoch's first allocation aliases these bytes.
    // A handle held across the advance therefore has to report itself expired rather than name storage that now
    // belongs to something else.
    auto const expiring = _transient_expiring.lock(
        [](cc::vector<std::weak_ptr<sg::raw_buffer const>>& v)
        {
            auto out = cc::move(v);
            v.clear();
            return out;
        });

    auto const expiring_textures = _transient_expiring_textures.lock(
        [](cc::vector<std::weak_ptr<sg::raw_texture const>>& v)
        {
            auto out = cc::move(v);
            v.clear();
            return out;
        });

    // Outside the lock: expire() runs the resource's finalizers, which stage a deferred release and take the epoch
    // system's own lock.
    for (auto const& weak : expiring)
        if (auto const buffer = weak.lock())
            buffer->expire();
    for (auto const& weak : expiring_textures)
        if (auto const texture = weak.lock())
            texture->expire();

    // The rings' bytes were read (or written) by copies recorded in the closing epoch, so they are only reclaimable
    // once that epoch retires.
    //
    // Where the head stands NOW is what that epoch owns; anything staged after this advance belongs to the next one
    // and must survive.
    // Rewinding to zero instead would hand those bytes out twice.
    auto const upload_mark = _upload_ring.lock([](metal_staging_ring& r) { return r.mark(); });
    auto const download_mark = _download_ring.lock([](metal_staging_ring& r) { return r.mark(); });
    _epochs.defer(
        [this, upload_mark, download_mark]
        {
            _upload_ring.lock([&](metal_staging_ring& r) { r.release_to(upload_mark); });
            _download_ring.lock([&](metal_staging_ring& r) { r.release_to(download_mark); });
        });

    _epochs.advance();
    apply_pending_transient_budget();
}

sg::submission_token metal_context::submit_command_list(std::unique_ptr<sg::command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot submit a null command list");
    auto& list = static_cast<metal_command_list&>(*cmd);
    CC_ASSERT(list.created_in_epoch() == current_epoch(), "a command list must be submitted in the epoch it was opened "
                                                          "in");

    auto const scope = autorelease_scope();

    list.end_recording();

    // Order this list after every off-frame transfer of a resource it touches.
    //
    // The two queues are otherwise independent: an async upload committed to the transfer queue has no relationship to
    // a command list committed to the direct one, so a list reading a buffer an upload is still filling would read
    // whatever was there.
    // One wait covers the whole list, on the highest value any of its resources claimed.
    if (auto const wait = highest_pending_transfer(list); wait > 0)
        _queue->wait(_transfers.timeline(), wait);

    // Finalize every buffer this list touched, in submission order — that ordering is what makes each resource's
    // `current` mean "after everything submitted so far".
    finalize_touched_buffers(list);

    // The token is claimed here rather than after the commit so the stamp below lands before submit returns: a caller
    // that issues an async transfer on the very next line must find this list already named.
    auto const token = _epochs.claim_submission_token();
    stamp_touched_resources(list, token);

    auto* const buffer = list.buffer();
    auto* const allocator = list.allocator();
    list.release_ownership();

    // Every commit carries a feedback handler, because it is the only channel Metal has for a failure that arrives
    // after the call that caused it — the validation layer speaks only to stderr.
    // The handler captures the sink rather than this context: it runs on a dispatch queue at a time nothing here
    // controls, which can be after shutdown.
    // See metal_feedback.hh.
    auto sink = _feedback;

    // The list's downloads: their bytes are in the staging ring and become readable when this commit completes, which
    // is precisely when the feedback handler runs.
    auto downloads = std::make_shared<cc::vector<cc::unique_function<void()>>>(list.take_pending_downloads());
    auto const has_downloads = !downloads->empty();
    auto* const pending_counter = &_pending_downloads;
    if (has_downloads)
        pending_counter->fetch_add(1, std::memory_order_acq_rel);

    auto* const options = MTL4::CommitOptions::alloc()->init();
    options->addFeedbackHandler(^void(MTL4::CommitFeedback* feedback) {
      auto* const error = feedback->error();
      if (error != nullptr)
          sink->report(device_error_kind_of(NS::UInteger(error->code())), describe_error(error, "a metal command "
                                                                                                "buffer failed"));

      for (auto& copy_out : *downloads)
          copy_out();
      downloads->clear();

      if (has_downloads)
          pending_counter->fetch_sub(1, std::memory_order_acq_rel);
    });

    MTL4::CommandBuffer const* const buffers[] = {buffer};
    _queue->commit(buffers, 1, options);
    options->release();

    _epochs.signal_submission(token);

    // The allocator rides the epoch rather than going back to the pool here: resetting it while the buffer just
    // committed is still executing is exactly what MTL4 forbids.
    _epochs.retire_allocator_with_epoch(allocator);
    _epochs.defer([buffer] { buffer->release(); });

    _slots.release(list.slot());

    return token;
}

void metal_context::drop_command_list(std::unique_ptr<sg::command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot drop a null command list");
    auto& list = static_cast<metal_command_list&>(*cmd);

    auto const scope = autorelease_scope();

    list.end_recording();

    auto* const buffer = list.buffer();
    auto* const allocator = list.allocator();
    list.release_ownership();

    // The list's work never runs, so every resource it declared against is left exactly as it was.
    for (auto const& touched : list.touched_buffers())
    {
        auto const& mtl_buffer = static_cast<metal_buffer const&>(*touched);
        mtl_buffer.access().lock([&](metal_resource_access& a) { a.discard(list.slot()); });
    }
    for (auto const& touched : list.touched_textures())
    {
        auto const& mtl_texture = static_cast<metal_texture const&>(*touched);
        mtl_texture.access().lock([&](metal_resource_access& a) { a.discard(list.slot()); });
    }

    // Nothing was committed, so the GPU never saw either object and both go back immediately.
    buffer->release();
    allocator->reset();
    _epochs.retire_allocator_with_epoch(allocator);

    _slots.release(list.slot());
}

u64 metal_context::highest_pending_transfer(metal_command_list& list) const
{
    u64 highest = 0;
    for (auto const& touched : list.touched_buffers())
        highest = cc::max(highest, _transfers.pending_value_for(*touched));
    for (auto const& touched : list.touched_textures())
        highest = cc::max(highest, _transfers.pending_value_for(*touched));
    return highest;
}

void metal_context::finalize_touched_buffers(metal_command_list& list)
{
    // Every touched buffer's state moves into its `current`, in submission order — that ordering is what makes
    // `current` mean "after everything submitted so far".
    //
    // The entry barrier each finalize computes is discarded rather than emitted: the list has already ordered its own
    // encoder against the queue (see metal_command_list::flush_barriers), and there is no way to prepend a barrier to
    // a command buffer that is already recorded.
    // Keeping the finalize is still what carries a write across lists, which is the half Metal genuinely needs.
    for (auto const& touched : list.touched_buffers())
    {
        auto const& mtl_buffer = static_cast<metal_buffer const&>(*touched);
        (void)mtl_buffer.access().lock([&](metal_resource_access& a) { return a.finalize(list.slot()); });
    }
    for (auto const& touched : list.touched_textures())
    {
        auto const& mtl_texture = static_cast<metal_texture const&>(*touched);
        (void)mtl_texture.access().lock([&](metal_resource_access& a) { return a.finalize(list.slot()); });
    }
}

void metal_context::stamp_touched_resources(metal_command_list& list, sg::submission_token token)
{
    for (auto const& touched : list.touched_buffers())
        static_cast<metal_buffer const&>(*touched).submission().raise(u64(token));
    for (auto const& touched : list.touched_textures())
        static_cast<metal_texture const&>(*touched).submission().raise(u64(token));
}

cc::result<std::unique_ptr<sg::command_list>> metal_context::try_create_command_list()
{
    if (is_device_lost())
        return cc::error("the metal device has been lost");

    auto const scope = autorelease_scope();

    auto* const allocator = _epochs.lease_allocator();
    auto* const buffer = _device->newCommandBuffer();
    if (buffer == nullptr)
    {
        _epochs.retire_allocator_with_epoch(allocator);
        return cc::error("the metal device refused a command buffer");
    }

    return std::unique_ptr<sg::command_list>(new metal_command_list(*this, current_epoch(), allocator, buffer));
}

void metal_context::shutdown()
{
    if (is_shut_down())
        return;

    auto const scope = autorelease_scope();

    // Routines first: they may cache epoch-managed resources that must be freed before the systems below go.
    routines.clear();

    // Both of these are device memory the context caches for its own lifetime, and Metal's reference counting does not
    // order them against the device — which is the category the vulkan build-out found the hard way, one validation
    // message at a time.
    release_transient_heap();
    release_cached_pipelines();

    // Close the final epoch and drain, so every deferred release runs while the device is still alive.
    advance_epoch();
    block_until_idle();

    // Before the device and the queue: a handler still in flight would otherwise report into a context being torn down.
    _feedback->detach();

    _epochs.shutdown();

    // After the drain above, so nothing in flight still names these bytes.
    _upload_ring.lock([](metal_staging_ring& r) { r.shutdown(); });
    _download_ring.lock([](metal_staging_ring& r) { r.shutdown(); });
    _transfers.shutdown();
    _samplers.shutdown();
    _texture_views.shutdown();

    if (_compiler != nullptr)
    {
        _compiler->release();
        _compiler = nullptr;
    }

    _residency.shutdown();

    _queue->release();
    _queue = nullptr;

    _device->release();
    _device = nullptr;

    // The flag directly rather than sg::context::shutdown(), which would re-run the three releases above — after the
    // device is gone, which is the wrong order even where they are idempotent.
    _is_shut_down = true;
}

void metal_context::shutdown_no_throw() noexcept
{
    // The teardown still has to run, and nothing above a destructor can act on a failure, so the only honest outcome is
    // to finish and say what happened.
    try
    {
        shutdown();
    }
    catch (sg::device_lost_exception const& e)
    {
        CC_LOG_ERROR("context shutdown on a lost device: {}", e.reason());
    }
    catch (sg::exception const& e)
    {
        CC_LOG_ERROR("context shutdown failed: {}", e.message());
    }
    catch (...)
    {
        CC_LOG_ERROR("context shutdown failed with an unknown exception");
    }
}

void metal_context::block_until_transfers_drained()
{
    // Both halves: an inline download's copy-out, and everything the off-frame queue still owes.
    while (_transfers.has_pending())
        std::this_thread::yield();

    // A condition rather than a duration: the handler runs on a dispatch queue we do not own, so there is nothing to
    // join and nothing whose timing is ours to predict.
    while (_pending_downloads.load(std::memory_order_acquire) > 0)
        std::this_thread::yield();
}

cc::result<sg::swapchain_handle> metal_context::try_create_swapchain(swapchain_description const& desc)
{
    return create_metal_swapchain(desc);
}

sg::texture_layout metal_context::async_ready_layout(async_direction) const
{
    // Metal textures have no layouts at all, so `general` is not a placeholder here the way it is on the other two
    // backends — it is the only thing a metal texture is ever in.
    return sg::texture_layout::general;
}

sg::texture_layout metal_context::current_texture_layout(raw_texture_handle const&, subresource_range const&) const
{
    // A Metal texture has no layout, so `general` is the true and only answer rather than a placeholder — see the
    // barrier translation, where a pure layout transition emits nothing at all.
    return sg::texture_layout::general;
}

void metal_context::async_upload_bytes_to_buffer(raw_buffer_handle buffer,
                                                 cc::pinned_data<byte const> data,
                                                 isize offset_in_bytes)
{
    _transfers.upload_to_buffer(cc::move(buffer), data, offset_in_bytes);
}

void metal_context::async_upload_bytes_to_texture(raw_texture_handle texture,
                                                  cc::pinned_data<byte const> data,
                                                  subresource_index const& subresource,
                                                  texture_region const& region)
{
    _transfers.upload_to_texture(cc::move(texture), data, subresource, region);
}

sg::bytes_future metal_context::async_download_bytes_from_buffer(raw_buffer_handle buffer,
                                                                 isize offset_in_bytes,
                                                                 isize size_in_bytes)
{
    return _transfers.download_from_buffer(cc::move(buffer), offset_in_bytes, size_in_bytes);
}

sg::bytes_future metal_context::async_download_bytes_from_texture(raw_texture_handle texture,
                                                                  subresource_index const& subresource,
                                                                  texture_region const& region)
{
    return _transfers.download_from_texture(cc::move(texture), subresource, region);
}

sg::stream_upload_handle metal_context::stream_bytes_to_buffer(raw_buffer_handle,
                                                               cc::pinned_data<byte const>,
                                                               isize,
                                                               stream_scope)
{
    SG_METAL_UNIMPLEMENTED("streaming bytes to a buffer");
}

sg::stream_upload_handle metal_context::stream_bytes_to_texture(raw_texture_handle,
                                                                cc::pinned_data<byte const>,
                                                                subresource_index const&,
                                                                texture_region const&,
                                                                stream_scope)
{
    SG_METAL_UNIMPLEMENTED("streaming bytes to a texture");
}

sg::stream_upload_handle metal_context::stream_source_to_buffer(raw_buffer_handle,
                                                                std::unique_ptr<stream_source>,
                                                                isize,
                                                                stream_scope)
{
    SG_METAL_UNIMPLEMENTED("streaming a source to a buffer");
}

sg::stream_upload_handle metal_context::stream_source_to_texture(raw_texture_handle,
                                                                 std::unique_ptr<stream_source>,
                                                                 subresource_index const&,
                                                                 texture_region const&,
                                                                 stream_scope)
{
    SG_METAL_UNIMPLEMENTED("streaming a source to a texture");
}

sg::stream_download_handle metal_context::stream_bytes_from_buffer(raw_buffer_handle, isize, isize, stream_scope)
{
    SG_METAL_UNIMPLEMENTED("streaming bytes from a buffer");
}

sg::stream_download_handle metal_context::stream_bytes_from_texture(raw_texture_handle,
                                                                    subresource_index const&,
                                                                    texture_region const&,
                                                                    stream_scope)
{
    SG_METAL_UNIMPLEMENTED("streaming bytes from a texture");
}

sg::stream_download_handle metal_context::stream_to_sink_from_buffer(raw_buffer_handle, stream_sink, isize, isize, stream_scope)
{
    SG_METAL_UNIMPLEMENTED("streaming a buffer to a sink");
}

sg::stream_download_handle metal_context::stream_to_sink_from_texture(raw_texture_handle,
                                                                      stream_sink,
                                                                      subresource_index const&,
                                                                      texture_region const&,
                                                                      stream_scope)
{
    SG_METAL_UNIMPLEMENTED("streaming a texture to a sink");
}

cc::result<sg::raw_buffer_handle> metal_context::try_create_raw_buffer(isize size_in_bytes,
                                                                       buffer_usages usage,
                                                                       allocation_info const& alloc)
{
    return cc::result<sg::raw_buffer_handle>(create_metal_buffer(size_in_bytes, usage, alloc));
}

cc::result<metal_buffer_handle> metal_context::create_metal_buffer(isize size_in_bytes,
                                                                   sg::buffer_usages usage,
                                                                   sg::allocation_info const& alloc)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");

    if (is_device_lost())
        return cc::error("the metal device has been lost");

    auto const scope = autorelease_scope();

    // An empty buffer allocates nothing: Metal refuses a zero length, and with validation armed the attempt aborts
    // rather than returning null.
    // A null MTLBuffer is the representation, and size 0 is a legal sg buffer.
    MTL::Buffer* buffer = nullptr;
    if (size_in_bytes > 0)
    {
        if (alloc.is_placed())
        {
            auto const& heap = static_cast<metal_memory_heap const&>(*alloc.heap);
            buffer = heap.heap()->newBuffer(NS::UInteger(size_in_bytes), k_buffer_options, NS::UInteger(alloc.offset));
            if (buffer == nullptr)
                return cc::error("the metal heap refused a placed buffer — check the offset's alignment and room");
        }
        else
        {
            buffer = _device->newBuffer(NS::UInteger(size_in_bytes), k_buffer_options);
            if (buffer == nullptr)
                return cc::error("the metal device refused a buffer allocation");
        }
    }

    // MTL4 names no resources at record time, so a buffer outside the residency set is simply absent when the GPU
    // runs — a copy from it reads zeroes, with nothing reported anywhere.
    _residency.add(buffer);

    auto handle = std::make_shared<metal_buffer const>(*this, size_in_bytes, usage, buffer, alloc.heap);

    if (alloc.scope == sg::lifetime_scope::transient)
        _transient_expiring.lock([&](cc::vector<std::weak_ptr<sg::raw_buffer const>>& v)
                                 { v.push_back(std::weak_ptr<sg::raw_buffer const>(handle)); });

    return handle;
}

cc::result<metal_memory_heap_handle> metal_context::create_metal_memory_heap(isize size_in_bytes)
{
    CC_ASSERT(size_in_bytes > 0, "heap size must be positive");

    if (is_device_lost())
        return cc::error("the metal device has been lost");

    auto const scope = autorelease_scope();

    auto* const descriptor = MTL::HeapDescriptor::alloc()->init();
    descriptor->setSize(NS::UInteger(size_in_bytes));
    descriptor->setStorageMode(MTL::StorageModePrivate);
    // Placement is the one heap type that lets the caller choose the offset, which is sg's whole model: an external
    // allocator sub-allocates and the heap only validates and mints.
    descriptor->setType(MTL::HeapTypePlacement);
    // Same reasoning as k_buffer_options: sg emits the barriers, so the driver must not infer its own.
    descriptor->setHazardTrackingMode(MTL::HazardTrackingModeUntracked);

    auto* const heap = _device->newHeap(descriptor);
    descriptor->release();

    if (heap == nullptr)
        return cc::error("the metal device refused a heap allocation");

    // A placement heap is the allocation; the buffers placed into it are not separately resident.
    _residency.add(heap);

    return std::make_shared<metal_memory_heap const>(*this, size_in_bytes, heap);
}

cc::result<sg::raw_texture_handle> metal_context::try_create_raw_texture(texture_description const& desc,
                                                                         allocation_info const& alloc)
{
    return cc::result<sg::raw_texture_handle>(create_metal_texture(desc, alloc));
}

cc::result<sg::memory_heap_handle> metal_context::try_create_memory_heap(isize size_in_bytes)
{
    return cc::result<sg::memory_heap_handle>(create_metal_memory_heap(size_in_bytes));
}

cc::result<sg::binding_group_layout_handle> metal_context::try_create_binding_group_layout(
    cc::span<binding const> bindings,
    cc::span<named_sampler const> static_samplers,
    lifetime_scope scope)
{
    return cc::result<sg::binding_group_layout_handle>(
        create_metal_binding_group_layout(bindings, static_samplers, scope));
}

cc::result<sg::pipeline_layout_handle> metal_context::try_create_pipeline_layout(pipeline_layout_description const& desc,
                                                                                 lifetime_scope scope)
{
    return cc::result<sg::pipeline_layout_handle>(create_metal_pipeline_layout(desc, scope));
}

cc::result<sg::compute_pipeline_handle> metal_context::try_create_compute_pipeline(compute_pipeline_description const& desc,
                                                                                   lifetime_scope scope)
{
    return cc::result<sg::compute_pipeline_handle>(create_metal_compute_pipeline(desc, scope));
}

cc::result<sg::raster_pipeline_handle> metal_context::try_create_raster_pipeline(raster_pipeline_description const& desc,
                                                                                 lifetime_scope scope)
{
    return cc::result<sg::raster_pipeline_handle>(create_metal_raster_pipeline(desc, scope));
}

cc::result<sg::raytracing_pipeline_handle> metal_context::try_create_raytracing_pipeline(
    raytracing_pipeline_description const&,
    lifetime_scope)
{
    return cc::error("the metal backend cannot create ray-tracing pipelines yet");
}

cc::result<sg::raytracing_shader_table_handle> metal_context::try_create_raytracing_shader_table(
    raytracing_shader_table_description const&,
    lifetime_scope)
{
    return cc::error("the metal backend cannot create ray-tracing shader tables yet");
}

cc::result<sg::binding_group_handle> metal_context::try_create_binding_group(binding_group_layout_handle layout,
                                                                             cc::span<named_view const> views,
                                                                             cc::span<named_sampler const> samplers,
                                                                             lifetime_scope scope)
{
    return cc::result<sg::binding_group_handle>(create_metal_binding_group(layout, views, samplers, scope));
}

cc::result<sg::staging_binding_group_handle> metal_context::try_create_staging_binding_group(binding_group_layout_handle layout,
                                                                                             lifetime_scope scope)
{
    return create_metal_staging_binding_group(cc::move(layout), scope);
}
} // namespace sg::backend::metal
