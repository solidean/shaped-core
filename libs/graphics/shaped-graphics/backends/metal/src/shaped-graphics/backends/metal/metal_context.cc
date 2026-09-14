#include "metal_context.hh"

#include <clean-core/common/assert.hh>
#include <clean-core/record/log.hh>
#include <shaped-graphics/exceptions.hh>

// Seams the milestone order has not reached; see libs/graphics/shaped-graphics/docs/writing-a-backend.md.
#define SG_METAL_UNIMPLEMENTED(what) CC_UNREACHABLE(what " is not implemented in the metal backend yet")

namespace sg::backend::metal
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "sg.metal");

metal_context::metal_context(MTL::Device* device,
                             MTL4::CommandQueue* queue,
                             MTL::SharedEvent* epoch_event,
                             MTL::SharedEvent* submission_event)
  : sg::context(sg::backend_kind::metal, sg::thread_model::multi_threaded, k_accepted_shader_formats),
    _device(device),
    _queue(queue),
    _epochs(device, queue, epoch_event, submission_event)
{
    CC_ASSERT(_device != nullptr && _queue != nullptr, "a metal context needs a device and a queue");
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
    case sg::feature::timestamp_query:
    case sg::feature::headless_present:
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

    auto* const buffer = list.buffer();
    auto* const allocator = list.allocator();
    list.release_ownership();

    MTL4::CommandBuffer const* const buffers[] = {buffer};
    _queue->commit(buffers, 1);

    auto const token = _epochs.claim_submission_token();
    _epochs.signal_submission(token);

    // The allocator rides the epoch rather than going back to the pool here: resetting it while the buffer just
    // committed is still executing is exactly what MTL4 forbids.
    _epochs.retire_allocator_with_epoch(allocator);
    _epochs.defer([buffer] { buffer->release(); });

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

    // Nothing was committed, so the GPU never saw either object and both go back immediately.
    buffer->release();
    allocator->reset();
    _epochs.retire_allocator_with_epoch(allocator);
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

    _epochs.shutdown();

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

cc::result<sg::swapchain_handle> metal_context::try_create_swapchain(swapchain_description const&)
{
    return cc::error("the metal backend cannot present yet");
}

sg::texture_layout metal_context::async_ready_layout(async_direction) const
{
    // Metal textures have no layouts at all, so `general` is not a placeholder here the way it is on the other two
    // backends — it is the only thing a metal texture is ever in.
    return sg::texture_layout::general;
}

sg::texture_layout metal_context::current_texture_layout(raw_texture_handle const&, subresource_range const&) const
{
    return sg::texture_layout::general;
}

void metal_context::async_upload_bytes_to_buffer(raw_buffer_handle, cc::pinned_data<byte const>, isize)
{
    SG_METAL_UNIMPLEMENTED("async buffer upload");
}

void metal_context::async_upload_bytes_to_texture(raw_texture_handle,
                                                  cc::pinned_data<byte const>,
                                                  subresource_index const&,
                                                  texture_region const&)
{
    SG_METAL_UNIMPLEMENTED("async texture upload");
}

sg::bytes_future metal_context::async_download_bytes_from_buffer(raw_buffer_handle, isize, isize)
{
    SG_METAL_UNIMPLEMENTED("async buffer download");
}

sg::bytes_future metal_context::async_download_bytes_from_texture(raw_texture_handle,
                                                                  subresource_index const&,
                                                                  texture_region const&)
{
    SG_METAL_UNIMPLEMENTED("async texture download");
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

cc::result<sg::raw_buffer_handle> metal_context::try_create_raw_buffer(isize, buffer_usages, allocation_info const&)
{
    return cc::error("the metal backend cannot create buffers yet");
}

cc::result<sg::raw_texture_handle> metal_context::try_create_raw_texture(texture_description const&,
                                                                         allocation_info const&)
{
    return cc::error("the metal backend cannot create textures yet");
}

cc::result<sg::memory_heap_handle> metal_context::try_create_memory_heap(isize)
{
    return cc::error("the metal backend cannot create memory heaps yet");
}

cc::result<sg::binding_group_layout_handle> metal_context::try_create_binding_group_layout(cc::span<binding const>,
                                                                                           cc::span<named_sampler const>,
                                                                                           lifetime_scope)
{
    return cc::error("the metal backend cannot create binding group layouts yet");
}

cc::result<sg::pipeline_layout_handle> metal_context::try_create_pipeline_layout(pipeline_layout_description const&,
                                                                                 lifetime_scope)
{
    return cc::error("the metal backend cannot create pipeline layouts yet");
}

cc::result<sg::compute_pipeline_handle> metal_context::try_create_compute_pipeline(compute_pipeline_description const&,
                                                                                   lifetime_scope)
{
    return cc::error("the metal backend cannot create compute pipelines yet");
}

cc::result<sg::raster_pipeline_handle> metal_context::try_create_raster_pipeline(raster_pipeline_description const&,
                                                                                 lifetime_scope)
{
    return cc::error("the metal backend cannot create raster pipelines yet");
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

cc::result<sg::binding_group_handle> metal_context::try_create_binding_group(binding_group_layout_handle,
                                                                             cc::span<named_view const>,
                                                                             cc::span<named_sampler const>,
                                                                             lifetime_scope)
{
    return cc::error("the metal backend cannot create binding groups yet");
}

cc::result<sg::staging_binding_group_handle> metal_context::try_create_staging_binding_group(binding_group_layout_handle,
                                                                                             lifetime_scope)
{
    return cc::error("the metal backend cannot create staging binding groups yet");
}
} // namespace sg::backend::metal
