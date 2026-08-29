#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_binding_group_layout.hh>
#include <shaped-graphics/backends/vulkan/vulkan_buffer.hh>
#include <shaped-graphics/backends/vulkan/vulkan_command_list.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/backends/vulkan/vulkan_completion_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_compute_pipeline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_descriptor_functions.hh>
#include <shaped-graphics/backends/vulkan/vulkan_descriptor_heap.hh>
#include <shaped-graphics/backends/vulkan/vulkan_download_async.hh>
#include <shaped-graphics/backends/vulkan/vulkan_download_inline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_epoch.hh>
#include <shaped-graphics/backends/vulkan/vulkan_memory_heap.hh>
#include <shaped-graphics/backends/vulkan/vulkan_pipeline_layout.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raster_pipeline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raytracing_functions.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raytracing_pipeline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_raytracing_shader_table.hh>
#include <shaped-graphics/backends/vulkan/vulkan_sampler.hh>
#include <shaped-graphics/backends/vulkan/vulkan_staging_binding_group.hh>
#include <shaped-graphics/backends/vulkan/vulkan_swapchain.hh>
#include <shaped-graphics/backends/vulkan/vulkan_texture.hh>
#include <shaped-graphics/backends/vulkan/vulkan_upload_async.hh>
#include <shaped-graphics/backends/vulkan/vulkan_upload_inline.hh>
#include <shaped-graphics/backends/vulkan/vulkan_view_desc.hh>
#include <shaped-graphics/binding/compiled_shader.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/memory/allocation_info.hh>

#include <atomic>

/// Per-backend creation config for the Vulkan context.
/// The two flags are independent.
struct sg::backend::vulkan::vulkan_config
{
    /// Enable the Khronos validation layer plus a debug messenger for its messages.
    /// Messages reach set_message_callback when one is installed, and the recording log otherwise — not stderr directly.
    /// Best-effort — skipped if the layer / VK_EXT_debug_utils isn't installed.
    /// The analogue of dx12_config::enable_debug_layer, and off by default for the same reason: it costs real time.
    bool enable_validation_layers = false;

    /// Prefer a software (CPU) physical device, e.g. lavapipe.
    /// Only a preference: Vulkan has no guaranteed software device, so this still falls back to hardware when none is present.
    bool prefer_software_device = false;

    /// Capacity of the staging ring behind cmd.upload, in bytes.
    /// One epoch's inline uploads must fit, since the ring is only reclaimed when an epoch retires.
    /// Matches the dx12 backend's default.
    isize upload_ring_bytes = 16 * 1024 * 1024;

    /// Capacity of the readback ring behind cmd.download, in bytes.
    isize download_ring_bytes = 16 * 1024 * 1024;

    /// Staging window for ctx.upload's transfer-queue system, in bytes.
    /// Three of these are allocated, so CPU staging and GPU copy overlap; an upload larger than one packs across
    /// successive windows.
    isize async_upload_window_bytes = 4 * 1024 * 1024;

    /// The same, for ctx.download's readback windows.
    isize async_download_window_bytes = 4 * 1024 * 1024;

    /// Capacity of the descriptor heap, in bytes, and the share of it reserved for transient binding groups.
    /// Sized in bytes rather than in descriptors because a descriptor's size is a device property.
    isize descriptor_heap_bytes = 4 * 1024 * 1024;
    float descriptor_transient_fraction = 0.25f;
};

/// Severity of a validation-layer message, mapped from VkDebugUtilsMessageSeverityFlagBitsEXT.
/// Ordered worst-first so `severity <= vulkan_message_severity::warning` reads as "warning or worse",
/// matching how the dx12 backend's listener is written.
enum class sg::backend::vulkan::vulkan_message_severity
{
    error,
    warning,
    info,
    verbose,
};

/// Vulkan implementation of sg::context.
/// The sg::context virtuals are thin forwarders to the backend-typed create_vulkan_* methods — prefer those when you hold a vulkan_context.
/// Bodies live in the sibling vulkan_*.cc files.
class sg::backend::vulkan::vulkan_context final : public sg::context
{
    // vulkan consumes SPIR-V only.
    static constexpr sg::shader_format k_accepted_shader_formats[] = {sg::shader_format::spirv};

public:
    vulkan_context(VkInstance instance,
                   VkPhysicalDevice physical_device,
                   VkDevice device,
                   VkQueue queue,
                   u32 queue_family_index,
                   VkSemaphore epoch_timeline,
                   VkSemaphore submission_timeline,
                   VkDebugUtilsMessengerEXT debug_messenger)
      : sg::context(sg::backend_kind::vulkan, sg::thread_model::multi_threaded, k_accepted_shader_formats),
        _instance(instance),
        _physical_device(physical_device),
        _device(device),
        _queue(queue),
        _queue_family_index(queue_family_index),
        _epoch_timeline(epoch_timeline),
        _submission_timeline(submission_timeline),
        _debug_messenger(debug_messenger)
    {
        vkGetPhysicalDeviceMemoryProperties(_physical_device, &_memory_properties);
    }

    ~vulkan_context() override { shutdown(); } // runs shutdown() before the base dtor asserts it

    // create_vulkan_context fills this in once it has picked a physical device.
    using sg::context::set_adapter_info;

    /// Whether this device has the ray-tracing extensions, which are optional above the required floor.
    /// Read by every command list's cmd.raytracing.is_supported(), so a device without them reports honestly
    /// rather than the backend hardcoding an answer.
    [[nodiscard]] bool is_raytracing_supported() const { return _raytracing_supported; }

    // Set once by create_vulkan_context, before the context is handed out; never changes afterwards.
    void set_raytracing_supported(bool supported) { _raytracing_supported = supported; }

    /// Serializes every queue operation on this device.
    ///
    /// **Vulkan queues are externally synchronized; D3D12's are free-threaded.**
    /// That is invisible until a second thread submits — and async transfer is exactly that, with two copy actors
    /// submitting while the app submits graphics work.
    /// Per-queue locks would cover vkQueueSubmit, but vkDeviceWaitIdle needs *every* queue at once, so one device-wide
    /// guard is both simpler and the only thing that makes an idle-wait safe.
    ///
    /// Contention is not the concern it looks: a submit is a short call, and the work it queues runs outside the lock.
    [[nodiscard]] cc::mutex<int>& queue_guard() const { return _queue_guard; }

    /// The queues async transfer runs on, and the family they belong to.
    ///
    /// Upload and download want separate queues, because a wait stalls everything behind it in a queue's FIFO — an
    /// upload deferred behind a graphics submission would otherwise hold up an unrelated download queued after it.
    /// Where the device could not give two they are the same queue, and where it could give none they are the
    /// graphics queue: async transfer stays correct either way, and only its concurrency degrades.
    [[nodiscard]] VkQueue upload_queue() const { return _upload_queue; }
    [[nodiscard]] VkQueue download_queue() const { return _download_queue; }
    [[nodiscard]] u32 transfer_queue_family() const { return _transfer_queue_family; }

    /// Whether the transfer queues are genuinely separate from the graphics queue.
    /// False means a copy competes with rendering for the same queue, which is worth knowing before blaming a
    /// transfer for a frame-time spike.
    [[nodiscard]] bool has_dedicated_transfer_queue() const { return _transfer_queue_family != _queue_family_index; }

    void set_transfer_queues(u32 family, VkQueue upload, VkQueue download, u32 effective_family)
    {
        _transfer_queue_family = effective_family;
        _upload_queue = upload;
        _download_queue = download;
        (void)family;
    }

    /// Whether this context can create a swapchain at all, and whether it can do so without a window.
    /// Both are instance/device facts settled at creation, so a create_swapchain failure is reported rather than
    /// discovered at the first present.
    [[nodiscard]] bool is_swapchain_supported() const { return _swapchain_supported; }
    [[nodiscard]] bool is_headless_present_supported() const { return _headless_surface_supported; }

    void set_presentation_support(bool swapchain, bool headless_surface)
    {
        _swapchain_supported = swapchain;
        _headless_surface_supported = headless_surface;
    }

    /// Descriptor sizes and offset alignment for this device, read once at creation.
    /// The bind path sizes every descriptor range from these, since a descriptor's size is a device property rather
    /// than something the API fixes.
    [[nodiscard]] VkPhysicalDeviceDescriptorBufferPropertiesEXT const& descriptor_buffer_properties() const
    {
        return _descriptor_buffer_properties;
    }

    /// The device's core properties, read once at creation.
    /// The pipeline path needs the vendor, device and cache UUID to tell whether a serialized cache blob is one this
    /// device will actually use.
    [[nodiscard]] VkPhysicalDeviceProperties const& device_properties() const { return _device_properties; }

    void set_device_properties(VkPhysicalDeviceProperties const& props) { _device_properties = props; }

    /// The device's acceleration-structure limits, read once at creation.
    /// A build needs the scratch alignment from here, which is a device property rather than a portable constant.
    [[nodiscard]] VkPhysicalDeviceAccelerationStructurePropertiesKHR const& acceleration_structure_properties() const
    {
        return _acceleration_structure_properties;
    }

    /// The device's ray-tracing pipeline limits — shader-group handle size and the shader-table alignments.
    [[nodiscard]] VkPhysicalDeviceRayTracingPipelinePropertiesKHR const& raytracing_pipeline_properties() const
    {
        return _raytracing_pipeline_properties;
    }

    void set_raytracing_properties(VkPhysicalDeviceAccelerationStructurePropertiesKHR const& accel,
                                   VkPhysicalDeviceRayTracingPipelinePropertiesKHR const& pipeline)
    {
        _acceleration_structure_properties = accel;
        _raytracing_pipeline_properties = pipeline;
        _acceleration_structure_properties.pNext = nullptr; // the chain it was queried through does not outlive creation
        _raytracing_pipeline_properties.pNext = nullptr;
    }

    void set_descriptor_buffer_properties(VkPhysicalDeviceDescriptorBufferPropertiesEXT const& props)
    {
        _descriptor_buffer_properties = props;
        _descriptor_buffer_properties.pNext = nullptr; // the chain it was queried through does not outlive creation
    }

    /// Routes this instance's validation messages to `callback` instead of the log.
    /// Only ever called when the context was created with enable_validation_layers, and only for messages raised
    /// after creation returned — the instance's own create/destroy messages go to the log either way.
    /// The layer raises a message on whatever thread provoked it, and this setter is not synchronized against that:
    /// set it before the context is driven from a second thread.
    /// An empty function restores the log default.
    ///
    /// Unlike dx12's, this is genuinely per-context: a Vulkan messenger belongs to one VkInstance and delivers only
    /// that instance's messages, so silencing one context leaves every other context's listener untouched.
    /// dx12 needs a thread-scoped guard instead because D3D12 was observed handing one message to every callback in
    /// the process; that is a runtime behaviour of its debug layer rather than a difference in how the two register.
    void set_message_callback(cc::unique_function<void(vulkan_message_severity, cc::string_view)> callback)
    {
        _message_callback = cc::move(callback);
    }

    // Set by create_vulkan_context once the context exists, so the messenger can carry it as user data.
    void set_debug_messenger(VkDebugUtilsMessengerEXT messenger) { _debug_messenger = messenger; }

    // Delivers one validation message to the installed callback, or to the log when none is installed.
    // Called from the debug messenger; body in vulkan_context.cc.
    void dispatch_validation_message(vulkan_message_severity severity, cc::string_view message) const;

    // backend-typed API — prefer these when you already hold a vulkan_context

    [[nodiscard]] cc::result<std::unique_ptr<vulkan_command_list>> create_vulkan_command_list();
    /// `extra_usage` is backend-internal, for a Vulkan usage sg has no vocabulary for.
    /// The one caller is the ray-tracing shader table, whose buffer must carry SHADER_BINDING_TABLE_BIT_KHR — a bit
    /// sg::buffer_usage deliberately does not model, since the table is its own abstraction rather than a buffer kind
    /// (see the note in shaped-graphics/types.hh).
    [[nodiscard]] cc::result<vulkan_buffer_handle> create_vulkan_buffer(isize size_in_bytes,
                                                                        sg::buffer_usages usage,
                                                                        sg::allocation_info const& alloc,
                                                                        VkBufferUsageFlags extra_usage = 0);
    [[nodiscard]] cc::result<vulkan_texture_handle> create_vulkan_texture(sg::texture_description const& desc,
                                                                          sg::allocation_info const& alloc);
    sg::submission_token submit_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd);
    void drop_vulkan_command_list(std::unique_ptr<vulkan_command_list> cmd);

    // The drop cleanup on an unsubmitted list: return its pool, release its slot, drop the open-list count.
    // Shared by drop_vulkan_command_list and the destructor's auto-drop — do not call directly, go through drop_vulkan_command_list.
    void reclaim_unsubmitted_command_list(vulkan_command_list& cmd);

    // sg::context overrides — forward to the backend-typed methods above.
    // The static_cast down is sound: backends never mix.

    [[nodiscard]] cc::result<std::unique_ptr<sg::command_list>> try_create_command_list() override
    {
        return cc::result<std::unique_ptr<sg::command_list>>(create_vulkan_command_list());
    }

    [[nodiscard]] cc::result<sg::raw_buffer_handle> try_create_raw_buffer(isize size_in_bytes,
                                                                          sg::buffer_usages usage,
                                                                          sg::allocation_info const& alloc) override
    {
        return cc::result<sg::raw_buffer_handle>(create_vulkan_buffer(size_in_bytes, usage, alloc));
    }

    [[nodiscard]] cc::result<sg::raw_texture_handle> try_create_raw_texture(sg::texture_description const& desc,
                                                                            sg::allocation_info const& alloc) override
    {
        return cc::result<sg::raw_texture_handle>(create_vulkan_texture(desc, alloc));
    }

    // Not implemented yet.
    // These return an error rather than aborting, so the sg throwing façade turns each into a typed exception.
    [[nodiscard]] cc::result<vulkan_memory_heap_handle> create_vulkan_memory_heap(isize size_in_bytes);

    [[nodiscard]] cc::result<sg::memory_heap_handle> try_create_memory_heap(isize size_in_bytes) override
    {
        return cc::result<sg::memory_heap_handle>(create_vulkan_memory_heap(size_in_bytes));
    }

    [[nodiscard]] cc::result<vulkan_swapchain_handle> create_vulkan_swapchain(sg::swapchain_description const& desc);

    [[nodiscard]] cc::result<sg::swapchain_handle> try_create_swapchain(sg::swapchain_description const& desc) override
    {
        return cc::result<sg::swapchain_handle>(create_vulkan_swapchain(desc));
    }

    // The bind path and the pipelines — not implemented yet.
    [[nodiscard]] cc::result<vulkan_binding_group_layout_handle> create_vulkan_binding_group_layout(
        cc::span<sg::binding const> bindings,
        cc::span<sg::named_sampler const> static_samplers);

    [[nodiscard]] cc::result<sg::binding_group_layout_handle> try_create_binding_group_layout(
        cc::span<sg::binding const> bindings,
        cc::span<sg::named_sampler const> static_samplers,
        sg::lifetime_scope scope) override
    {
        // Layouts are cached schemas, so a transient one is a category error rather than an unimplemented case.
        CC_ASSERT(scope == sg::lifetime_scope::persistent, "binding group layouts are persistent-only");
        return cc::result<sg::binding_group_layout_handle>(create_vulkan_binding_group_layout(bindings, static_samplers));
    }
    [[nodiscard]] cc::result<vulkan_pipeline_layout_handle> create_vulkan_pipeline_layout(
        sg::pipeline_layout_description const& desc);

    [[nodiscard]] cc::result<sg::pipeline_layout_handle> try_create_pipeline_layout(
        sg::pipeline_layout_description const& desc,
        sg::lifetime_scope scope) override
    {
        CC_ASSERT(scope == sg::lifetime_scope::persistent, "pipeline layouts are persistent-only");
        return cc::result<sg::pipeline_layout_handle>(create_vulkan_pipeline_layout(desc));
    }
    [[nodiscard]] cc::result<vulkan_compute_pipeline_handle> create_vulkan_compute_pipeline(
        sg::compute_pipeline_description const& desc);

    [[nodiscard]] cc::result<sg::compute_pipeline_handle> try_create_compute_pipeline(
        sg::compute_pipeline_description const& desc,
        sg::lifetime_scope scope) override
    {
        CC_ASSERT(scope == sg::lifetime_scope::persistent, "pipelines are persistent-only");
        return cc::result<sg::compute_pipeline_handle>(create_vulkan_compute_pipeline(desc));
    }
    [[nodiscard]] cc::result<vulkan_raster_pipeline_handle> create_vulkan_raster_pipeline(
        sg::raster_pipeline_description const& desc);

    [[nodiscard]] cc::result<sg::raster_pipeline_handle> try_create_raster_pipeline(
        sg::raster_pipeline_description const& desc,
        sg::lifetime_scope scope) override
    {
        CC_ASSERT(scope == sg::lifetime_scope::persistent, "pipelines are persistent-only");
        return cc::result<sg::raster_pipeline_handle>(create_vulkan_raster_pipeline(desc));
    }
    [[nodiscard]] cc::result<vulkan_raytracing_pipeline_handle> create_vulkan_raytracing_pipeline(
        sg::raytracing_pipeline_description const& desc);

    [[nodiscard]] cc::result<sg::raytracing_pipeline_handle> try_create_raytracing_pipeline(
        sg::raytracing_pipeline_description const& desc,
        sg::lifetime_scope scope) override
    {
        CC_ASSERT(scope == sg::lifetime_scope::persistent, "pipelines are persistent-only");
        return cc::result<sg::raytracing_pipeline_handle>(create_vulkan_raytracing_pipeline(desc));
    }
    [[nodiscard]] cc::result<vulkan_raytracing_shader_table_handle> create_vulkan_raytracing_shader_table(
        sg::raytracing_shader_table_description const& desc);

    [[nodiscard]] cc::result<sg::raytracing_shader_table_handle> try_create_raytracing_shader_table(
        sg::raytracing_shader_table_description const& desc,
        sg::lifetime_scope scope) override
    {
        CC_ASSERT(scope == sg::lifetime_scope::persistent, "shader tables are persistent-only");
        return cc::result<sg::raytracing_shader_table_handle>(create_vulkan_raytracing_shader_table(desc));
    }
    [[nodiscard]] cc::result<vulkan_binding_group_handle> create_vulkan_binding_group(
        sg::binding_group_layout_handle const& layout,
        cc::span<sg::named_view const> views,
        cc::span<sg::named_sampler const> samplers,
        sg::lifetime_scope scope);

    [[nodiscard]] cc::result<sg::binding_group_handle> try_create_binding_group(sg::binding_group_layout_handle layout,
                                                                                cc::span<sg::named_view const> views,
                                                                                cc::span<sg::named_sampler const> samplers,
                                                                                sg::lifetime_scope scope) override
    {
        return cc::result<sg::binding_group_handle>(create_vulkan_binding_group(layout, views, samplers, scope));
    }
    [[nodiscard]] cc::result<vulkan_staging_binding_group_handle> create_vulkan_staging_binding_group(
        sg::binding_group_layout_handle const& layout,
        sg::lifetime_scope scope);

    [[nodiscard]] cc::result<sg::staging_binding_group_handle> try_create_staging_binding_group(
        sg::binding_group_layout_handle layout,
        sg::lifetime_scope scope) override
    {
        return cc::result<sg::staging_binding_group_handle>(create_vulkan_staging_binding_group(layout, scope));
    }

    sg::submission_token submit_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        return submit_vulkan_command_list(
            std::unique_ptr<vulkan_command_list>(static_cast<vulkan_command_list*>(cmd.release())));
    }

    void drop_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        drop_vulkan_command_list(std::unique_ptr<vulkan_command_list>(static_cast<vulkan_command_list*>(cmd.release())));
    }

    void async_upload_bytes_to_buffer(sg::raw_buffer_handle buffer, cc::pinned_data<byte const> data, isize offset) override
    {
        _upload_async.upload_buffer(buffer, cc::move(data), offset);
    }
    void async_upload_bytes_to_texture(sg::raw_texture_handle,
                                       cc::pinned_data<byte const>,
                                       sg::subresource_index const&,
                                       sg::texture_region const&) override
    {
        CC_UNREACHABLE("vulkan async texture upload is not implemented yet");
    }

    [[nodiscard]] sg::bytes_future async_download_bytes_from_buffer(sg::raw_buffer_handle buffer,
                                                                    isize offset,
                                                                    isize size_in_bytes) override
    {
        return _download_async.download_buffer(buffer, offset, size_in_bytes);
    }
    [[nodiscard]] sg::bytes_future async_download_bytes_from_texture(sg::raw_texture_handle,
                                                                     sg::subresource_index const&,
                                                                     sg::texture_region const&) override
    {
        CC_UNREACHABLE("vulkan async texture download is not implemented yet");
    }

    // Streaming transfers (ctx.stream) — not implemented yet, and aborting for the same reason as the async pair:
    // a handle whose completion nothing will ever settle parks its dependents forever, which is worse than a stop.
    [[nodiscard]] sg::stream_upload_handle stream_bytes_to_buffer(sg::raw_buffer_handle,
                                                                  cc::pinned_data<byte const>,
                                                                  isize,
                                                                  sg::stream_scope) override
    {
        CC_UNREACHABLE("vulkan streaming upload is not implemented yet");
    }
    [[nodiscard]] sg::stream_upload_handle stream_bytes_to_texture(sg::raw_texture_handle,
                                                                   cc::pinned_data<byte const>,
                                                                   sg::subresource_index const&,
                                                                   sg::texture_region const&,
                                                                   sg::stream_scope) override
    {
        CC_UNREACHABLE("vulkan streaming texture upload is not implemented yet");
    }
    [[nodiscard]] sg::stream_upload_handle stream_source_to_buffer(sg::raw_buffer_handle,
                                                                   std::unique_ptr<sg::stream_source>,
                                                                   isize,
                                                                   sg::stream_scope) override
    {
        CC_UNREACHABLE("vulkan source-driven streaming upload is not implemented yet");
    }
    [[nodiscard]] sg::stream_upload_handle stream_source_to_texture(sg::raw_texture_handle,
                                                                    std::unique_ptr<sg::stream_source>,
                                                                    sg::subresource_index const&,
                                                                    sg::texture_region const&,
                                                                    sg::stream_scope) override
    {
        CC_UNREACHABLE("vulkan source-driven streaming texture upload is not implemented yet");
    }
    [[nodiscard]] sg::stream_download_handle stream_bytes_from_buffer(sg::raw_buffer_handle,
                                                                      isize,
                                                                      isize,
                                                                      sg::stream_scope) override
    {
        CC_UNREACHABLE("vulkan streaming download is not implemented yet");
    }
    [[nodiscard]] sg::stream_download_handle stream_bytes_from_texture(sg::raw_texture_handle,
                                                                       sg::subresource_index const&,
                                                                       sg::texture_region const&,
                                                                       sg::stream_scope) override
    {
        CC_UNREACHABLE("vulkan streaming texture download is not implemented yet");
    }

    [[nodiscard]] sg::stream_download_handle stream_to_sink_from_buffer(sg::raw_buffer_handle,
                                                                        sg::stream_sink,
                                                                        isize,
                                                                        isize,
                                                                        sg::stream_scope) override
    {
        CC_UNREACHABLE("vulkan sink-driven streaming download is not implemented yet");
    }
    [[nodiscard]] sg::stream_download_handle stream_to_sink_from_texture(sg::raw_texture_handle,
                                                                         sg::stream_sink,
                                                                         sg::subresource_index const&,
                                                                         sg::texture_region const&,
                                                                         sg::stream_scope) override
    {
        CC_UNREACHABLE("vulkan sink-driven streaming texture download is not implemented yet");
    }

    // Deferred deletion: a refcount-zero GPU resource, staged for the current epoch and freed once that epoch retires.
    // Called from ~vulkan_buffer and ~vulkan_texture; safe to call from any thread.
    void schedule_deferred_deletion(vulkan_expiring_resource expiring);

    // Epoch contract — bodies in vulkan_epoch.cc.
    // Realized on a pair of timeline semaphores: the epoch timeline gates reclamation, the submission timeline answers per-list queries.

    [[nodiscard]] sg::epoch current_epoch() const override { return _current_epoch; }
    [[nodiscard]] sg::epoch completed_epoch() const override;
    void advance_epoch(cc::optional<int> allowed_in_flight) override;
    void advance_epoch_and_wait_for_idle() override { advance_epoch(0); }
    void process_completed_epochs() override;
    void wait_for_epoch(sg::epoch e) override;
    void wait_for_next_inflight_epoch() override;

    /// Whether any submitted epoch has yet to retire.
    /// The inline rings ask before blocking: with nothing in flight, a full ring cannot be reclaimed by waiting, and
    /// the request is a budget error rather than back-pressure.
    [[nodiscard]] bool has_epochs_in_flight();

    /// Blocks until `token`'s command list has finished executing.
    /// The readback actor uses it after it has exhausted cooperative pumping; see vulkan_download_inline.hh.
    void wait_for_submission_token(sg::submission_token token);
    [[nodiscard]] bool is_submission_complete(sg::submission_token token) const override;

    void shutdown() override;

    // Index of a device memory type satisfying `type_bits` (from a requirements mask) and all of `properties`.
    // Returns UINT32_MAX if none matches.
    [[nodiscard]] u32 find_memory_type(u32 type_bits, VkMemoryPropertyFlags properties) const;

    // If `r` is VK_ERROR_DEVICE_LOST, records the sticky loss reason and returns true; `what` labels the failing op.
    // Call after any VkResult on the device timeline (submit, semaphore wait), then raise sg::device_lost_exception at the public boundary.
    // See sg::context::is_device_lost for the sticky-loss surface this feeds.
    // Body in vulkan_context.cc.
    bool note_device_lost_if_lost(VkResult r, char const* what);

    /// The staging ring behind cmd.upload; owned here because its space is reclaimed on the epoch cycle.
    vulkan_upload_inline_system _upload_inline;

    /// The transfer-queue systems behind ctx.upload / ctx.download, which are epoch-independent.
    vulkan_upload_async_system _upload_async;
    vulkan_download_async_system _download_async;

    /// The readback ring behind cmd.download, and the actor that drains it.
    vulkan_download_inline_system _download_inline;

    /// Registers a transient resource for auto-expiry at the next epoch advance, and hands it straight back.
    /// A pass-through so a creation path stays one return statement; a persistent resource passes through untouched.
    /// Bodies in vulkan_buffer.cc / vulkan_texture.cc, where the type is complete.
    [[nodiscard]] std::shared_ptr<vulkan_buffer> register_if_transient(std::shared_ptr<vulkan_buffer> buffer,
                                                                       sg::lifetime_scope scope);
    [[nodiscard]] std::shared_ptr<vulkan_texture> register_if_transient(std::shared_ptr<vulkan_texture> texture,
                                                                        sg::lifetime_scope scope);

    /// The transient resources of the open epoch, expired when it advances.
    /// Weak, so a registration never keeps a resource alive: a transient handle nobody holds is simply gone by then.
    mutable cc::mutex<cc::vector<std::weak_ptr<sg::raw_buffer const>>> _transient_expiring;
    mutable cc::mutex<cc::vector<std::weak_ptr<sg::raw_texture const>>> _transient_expiring_textures;

    /// Descriptors live here, and a binding group is a range of it.
    vulkan_descriptor_heap _descriptor_heap;

    /// The objects a descriptor can only name, rather than describe inline.
    /// Both are caches for the context's life: a view or sampler an sg value type describes has no lifetime of its
    /// own, and giving each group its own objects would mean deferring their destruction behind every group.
    vulkan_image_view_cache _image_views = vulkan_image_view_cache(*this);
    vulkan_sampler_cache _samplers = vulkan_sampler_cache(*this);

    /// The descriptor-buffer entry points, loaded once at creation.
    vulkan_descriptor_functions _descriptor_functions;

    /// The ray-tracing entry points; only loaded where the device has the extensions.
    vulkan_raytracing_functions _raytracing_functions;

    /// Hands out the per-resource transfer timelines; see vulkan_completion_group.
    vulkan_completion_group_pool _group_pool;

    // Set once at creation from the device's extension set; see is_raytracing_supported.
    bool _raytracing_supported = false;

    // See is_swapchain_supported / is_headless_present_supported.
    bool _swapchain_supported = false;
    bool _headless_surface_supported = false;

    // See queue_guard.
    // Mutable so a const context can still be submitted through.
    mutable cc::mutex<int> _queue_guard;

    // See upload_queue / download_queue.
    // Owned by the device, so nothing destroys them.
    VkQueue _upload_queue = VK_NULL_HANDLE;
    VkQueue _download_queue = VK_NULL_HANDLE;
    u32 _transfer_queue_family = 0;

    // See descriptor_buffer_properties.
    VkPhysicalDeviceDescriptorBufferPropertiesEXT _descriptor_buffer_properties = {};

    // See device_properties.
    VkPhysicalDeviceProperties _device_properties = {};

    // See acceleration_structure_properties / raytracing_pipeline_properties.
    VkPhysicalDeviceAccelerationStructurePropertiesKHR _acceleration_structure_properties = {};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR _raytracing_pipeline_properties = {};

    // Where validation messages go; empty means the log.
    // See set_message_callback.
    cc::unique_function<void(vulkan_message_severity, cc::string_view)> _message_callback;

    VkInstance _instance = VK_NULL_HANDLE;
    VkPhysicalDevice _physical_device = VK_NULL_HANDLE; // owned by the instance, not destroyed

    // The device's memory types, read once at construction: they never change, and a staging ring allocates far too
    // often to re-query them per allocation.
    VkPhysicalDeviceMemoryProperties _memory_properties = {};
    VkDevice _device = VK_NULL_HANDLE;
    VkQueue _queue = VK_NULL_HANDLE; // owned by the device, not destroyed
    u32 _queue_family_index = 0;

    // The epoch timeline is signaled with the epoch value at the end of each epoch.
    // The submission timeline carries a per-command-list value on the same queue.
    // Both are VK_SEMAPHORE_TYPE_TIMELINE, so completion is a counter read rather than a host event.
    VkSemaphore _epoch_timeline = VK_NULL_HANDLE;
    VkSemaphore _submission_timeline = VK_NULL_HANDLE;

    // Written only by advance (externally synchronized), read concurrently by create/submit/drop.
    sg::epoch _current_epoch = sg::epoch::first;

    // create / submit / drop are thread-safe, so the shared bookkeeping they touch is synchronized:
    //  - _open_command_lists: bumped per create, dropped per submit/drop — a lock-free counter.
    //  - _next_submission: the next completion token, guarded together with the vkQueueSubmit + signal so token order == queue/signal order.
    //  - _command_pools: the command-pool pool (see vulkan_command_pool_set).
    std::atomic<int> _open_command_lists = 0; // must reach 0 before advance — lists cannot span epochs
    // cc::mutex's value ctor is explicit, so the type is named rather than left to `= {…}`.
    cc::mutex<sg::submission_token> _next_submission = cc::mutex<sg::submission_token>(sg::submission_token::first);
    cc::mutex<vulkan_command_pool_set> _command_pools;

    // Hands each open command list a dense access-tracking slot, acquired at create and released at submit/drop.
    // Internally synchronized.
    sg::command_list_slot_allocator _command_list_slots;

    cc::mutex<vulkan_epoch_state> _epoch_state;

    VkDebugUtilsMessengerEXT _debug_messenger = VK_NULL_HANDLE; // VK_NULL_HANDLE when validation is off
};

namespace sg
{
/// Creates a context on the Vulkan backend.
/// Returns an error (never asserts) on environment failure: no driver, no device with a graphics queue, no timeline-semaphore support, a refused device or semaphore.
/// Lives in `sg` rather than sg::backend::vulkan so every backend shares the create_*_context prefix; sg itself neither depends on nor knows this backend.
[[nodiscard]] cc::result<context_handle> create_vulkan_context(backend::vulkan::vulkan_config const& config = {});
} // namespace sg
