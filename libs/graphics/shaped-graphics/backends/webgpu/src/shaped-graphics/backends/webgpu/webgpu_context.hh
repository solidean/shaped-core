#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/ringbuffer.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/thread/thread.hh>
#include <shaped-graphics/backends/webgpu/fwd.hh>
#include <shaped-graphics/backends/webgpu/webgpu_binding_group.hh>
#include <shaped-graphics/backends/webgpu/webgpu_binding_group_layout.hh>
#include <shaped-graphics/backends/webgpu/webgpu_buffer.hh>
#include <shaped-graphics/backends/webgpu/webgpu_command_list.hh>
#include <shaped-graphics/backends/webgpu/webgpu_common.hh>
#include <shaped-graphics/backends/webgpu/webgpu_constant_pages.hh>
#include <shaped-graphics/backends/webgpu/webgpu_memory_heap.hh>
#include <shaped-graphics/backends/webgpu/webgpu_pipeline.hh>
#include <shaped-graphics/backends/webgpu/webgpu_query.hh>
#include <shaped-graphics/backends/webgpu/webgpu_readback.hh>
#include <shaped-graphics/backends/webgpu/webgpu_sampler.hh>
#include <shaped-graphics/backends/webgpu/webgpu_stream.hh>
#include <shaped-graphics/backends/webgpu/webgpu_swapchain.hh>
#include <shaped-graphics/backends/webgpu/webgpu_texture.hh>
#include <shaped-graphics/backends/webgpu/webgpu_upload_ring.hh>
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/memory/allocation_info.hh>

/// Creation knobs for a WebGPU context.
struct sg::backend::webgpu::webgpu_config
{
    /// Ask for the low-power adapter rather than the high-performance one; only a hint to the browser.
    bool prefer_low_power = false;

    /// Capacity of the staging ring behind cmd.upload, in bytes.
    /// The ring rewinds whenever no open list holds a span, so it bounds one list's uploads rather than one epoch's.
    isize upload_ring_bytes = 16 * 1024 * 1024;

    /// Size of one inline-constants page.
    /// 64 KiB is WebGPU's default maxUniformBufferBindingSize, and the most one binding may address.
    isize constant_page_bytes = 64 * 1024;

    /// The most bytes one streaming window moves.
    isize stream_window_bytes = 4 * 1024 * 1024;
};

/// Everything a WebGPU callback reaches the context through.
///
/// A callback can arrive after the context is gone — a map or a work-done promise settling during teardown — so a callback holds this rather than the context, and shutdown nulls `ctx` first.
struct sg::backend::webgpu::webgpu_callback_anchor
{
    webgpu_context* ctx = nullptr;

    /// Where validation and out-of-memory errors go, in addition to take_pending_errors; see webgpu_context::set_message_callback.
    cc::unique_function<void(WGPUErrorType, cc::string_view)> on_error;
};

/// A released object awaiting the retire of the epoch it was released in.
struct sg::backend::webgpu::webgpu_expiring_resource
{
    wgpu_buffer buffer;
    wgpu_texture texture;
    cc::vector<cc::unique_function<void()>> finalizers;
};

/// Everything one epoch owns.
struct sg::backend::webgpu::webgpu_epoch_data
{
    sg::epoch epoch_id = sg::epoch::invalid;
    cc::vector<webgpu_expiring_resource> expiring;
};

/// The epoch bookkeeping: what the GPU has finished, and what is waiting on it.
///
/// Completion arrives through `queue.onSubmittedWorkDone`, registered once per submit and once per advance, so nothing here polls or waits.
struct sg::backend::webgpu::webgpu_epoch_state
{
    u64 completed_epoch = u64(sg::epoch::first) - 1;
    u64 completed_submission = u64(sg::submission_token::first) - 1;
    cc::ringbuffer<webgpu_epoch_data> in_flight;
    cc::vector<webgpu_expiring_resource> staged;
};

/// WebGPU implementation of sg::context, over emdawnwebgpu.
///
/// **It never blocks, and cannot**: a browser settles a promise only once the task holding the thread returns.
/// So `execution()` is `never_block`, every internal wait is unreachable, and completion flows from WebGPU's callbacks into `settle_due_completions` — `arm_completion_signal` has nothing to arm.
///
/// `main_thread`: the device lives in the main thread's JS realm, so the bound calls and every callback run there.
/// Asyncs, layouts and samplers stay free-threaded — asyncs move to main themselves, and layouts create their WebGPU objects on first use.
///
/// What WebGPU lacks is emulated or refused, per libs/graphics/shaped-graphics/backends/webgpu/readme.md.
/// Inline constants and register-bound samplers live in group 3, 1D textures are 2D, and heaps place nothing.
/// Ray tracing, binding arrays and staging binding groups are refused.
class sg::backend::webgpu::webgpu_context final : public sg::context
{
    static constexpr sg::shader_format k_accepted_shader_formats[] = {sg::shader_format::wgsl};

public:
    webgpu_context(wgpu_instance instance,
                   wgpu_adapter adapter,
                   wgpu_device device,
                   std::shared_ptr<webgpu_callback_anchor> anchor,
                   webgpu_config const& config);

    ~webgpu_context() override;

    using sg::context::mark_device_lost;
    using sg::context::report_device_error;
    using sg::context::set_adapter_info;
    using sg::context::settle_due_completions;

    [[nodiscard]] sg::execution_model execution() const override { return sg::execution_model::never_block; }

    [[nodiscard]] bool supports(sg::feature f) const override
    {
        switch (f)
        {
        case sg::feature::timestamp_query:
            return _queries.is_supported();
        case sg::feature::headless_present:
            return true;
        case sg::feature::readwrite_storage_formats:
            return _readwrite_storage_formats;
        case sg::feature::float32_filtering:
            return _float32_filtering;
        case sg::feature::extended_storage_formats:
            return _extended_storage_formats;
        case sg::feature::raytracing:
        case sg::feature::geometry_shader:
        case sg::feature::tessellation_shader:
        case sg::feature::binding_arrays:
            return false;
        case sg::feature::unaligned_block_compression:
            // Lifted by `texture-compression-unaligned`, which the emdawnwebgpu this builds against does not offer.
            return false;
        }
        return false;
    }

    /// Routes this device's validation and out-of-memory errors to `callback` too, as they arrive.
    /// Every error still lands on take_pending_errors; an empty function removes the callback.
    void set_message_callback(cc::unique_function<void(WGPUErrorType, cc::string_view)> callback)
    {
        _anchor->on_error = cc::move(callback);
    }

    /// Every path into the device and the queue passes here, so a call from the wrong thread asserts before WebGPU fails obscurely.
    [[nodiscard]] WGPUDevice device() const
    {
        assert_on_device_thread();
        return _device.get();
    }
    [[nodiscard]] WGPUQueue queue() const
    {
        assert_on_device_thread();
        return _queue.get();
    }
    [[nodiscard]] WGPUInstance instance() const { return _instance.get(); }
    [[nodiscard]] std::shared_ptr<webgpu_callback_anchor> const& anchor() const { return _anchor; }

    /// The adapter's minimum uniform offset alignment, which inline constants are placed at.
    [[nodiscard]] isize uniform_offset_alignment() const { return _uniform_offset_alignment; }


    // backend-typed API

    [[nodiscard]] cc::result<std::unique_ptr<webgpu_command_list>> create_webgpu_command_list();
    [[nodiscard]] cc::result<webgpu_buffer_handle> create_webgpu_buffer(isize size_in_bytes,
                                                                        sg::buffer_usages usage,
                                                                        sg::allocation_info const& alloc);
    [[nodiscard]] cc::result<webgpu_texture_handle> create_webgpu_texture(sg::texture_description const& desc,
                                                                          sg::allocation_info const& alloc);
    sg::submission_token submit_webgpu_command_list(std::unique_ptr<webgpu_command_list> cmd);
    void drop_webgpu_command_list(std::unique_ptr<webgpu_command_list> cmd);

    // The drop cleanup, shared by drop and the list's auto-drop.
    void reclaim_unsubmitted_command_list(webgpu_command_list& cmd);

    // sg::context overrides

    [[nodiscard]] cc::result<std::unique_ptr<sg::command_list>> try_create_command_list() override
    {
        return cc::result<std::unique_ptr<sg::command_list>>(create_webgpu_command_list());
    }
    [[nodiscard]] cc::result<sg::raw_buffer_handle> try_create_raw_buffer(isize size_in_bytes,
                                                                          sg::buffer_usages usage,
                                                                          sg::allocation_info const& alloc) override
    {
        return cc::result<sg::raw_buffer_handle>(create_webgpu_buffer(size_in_bytes, usage, alloc));
    }
    [[nodiscard]] cc::result<sg::raw_texture_handle> try_create_raw_texture(sg::texture_description const& desc,
                                                                            sg::allocation_info const& alloc) override
    {
        return cc::result<sg::raw_texture_handle>(create_webgpu_texture(desc, alloc));
    }
    [[nodiscard]] cc::result<sg::memory_heap_handle> try_create_memory_heap(isize size_in_bytes) override
    {
        CC_ASSERT(size_in_bytes >= 0, "heap size must be non-negative");
        return sg::memory_heap_handle(std::make_shared<webgpu_memory_heap>(size_in_bytes));
    }
    [[nodiscard]] cc::result<sg::swapchain_handle> try_create_swapchain(sg::swapchain_description const& desc) override
    {
        return cc::result<sg::swapchain_handle>(webgpu_swapchain::create(*this, desc));
    }

    [[nodiscard]] cc::result<sg::binding_group_layout_handle> try_create_binding_group_layout(
        cc::span<sg::binding const> bindings,
        cc::span<sg::named_sampler const> static_samplers,
        sg::lifetime_scope scope) override
    {
        CC_ASSERT(scope == sg::lifetime_scope::persistent, "binding group layouts are persistent-only");
        return cc::result<sg::binding_group_layout_handle>(
            webgpu_binding_group_layout::create(*this, bindings, static_samplers));
    }
    [[nodiscard]] cc::result<sg::pipeline_layout_handle> try_create_pipeline_layout(
        sg::pipeline_layout_description const& desc,
        sg::lifetime_scope scope) override
    {
        CC_ASSERT(scope == sg::lifetime_scope::persistent, "pipeline layouts are persistent-only");
        return cc::result<sg::pipeline_layout_handle>(webgpu_pipeline_layout::create(*this, desc));
    }
    [[nodiscard]] cc::result<sg::compute_pipeline_handle> try_create_compute_pipeline(
        sg::compute_pipeline_description const& desc,
        sg::lifetime_scope scope) override;
    [[nodiscard]] cc::result<sg::raster_pipeline_handle> try_create_raster_pipeline(
        sg::raster_pipeline_description const& desc,
        sg::lifetime_scope scope) override;

    /// Settled from WebGPU's own asynchronous pipeline compile, so no thread is occupied while it runs.
    [[nodiscard]] cc::shared_async<sg::compute_pipeline_handle> create_compute_pipeline_async(
        sg::compute_pipeline_description const& desc,
        sg::lifetime_scope scope) override;
    [[nodiscard]] cc::shared_async<sg::raster_pipeline_handle> create_raster_pipeline_async(
        sg::raster_pipeline_description const& desc,
        sg::lifetime_scope scope) override;

    [[nodiscard]] cc::result<sg::raytracing_pipeline_handle> try_create_raytracing_pipeline(
        sg::raytracing_pipeline_description const&,
        sg::lifetime_scope) override
    {
        return cc::error("webgpu has no ray tracing");
    }
    [[nodiscard]] cc::result<sg::raytracing_shader_table_handle> try_create_raytracing_shader_table(
        sg::raytracing_shader_table_description const&,
        sg::lifetime_scope) override
    {
        return cc::error("webgpu has no ray tracing");
    }

    [[nodiscard]] cc::result<sg::binding_group_handle> try_create_binding_group(sg::binding_group_layout_handle layout,
                                                                                cc::span<sg::named_view const> views,
                                                                                cc::span<sg::named_sampler const> samplers,
                                                                                sg::lifetime_scope scope) override
    {
        return cc::result<sg::binding_group_handle>(
            webgpu_binding_group::create(*this, as_webgpu_layout(layout), views, samplers, scope));
    }
    [[nodiscard]] cc::result<sg::binding_group_handle> try_create_binding_group(sg::binding_group_layout_handle layout,
                                                                                cc::span<sg::slotted_view const> views,
                                                                                cc::span<sg::named_sampler const> samplers,
                                                                                sg::lifetime_scope scope) override
    {
        return cc::result<sg::binding_group_handle>(
            webgpu_binding_group::create(*this, as_webgpu_layout(layout), views, samplers, scope));
    }

    /// A staging group is a mutable array of descriptors, and WebGPU core has neither arrays nor mutable groups.
    [[nodiscard]] cc::result<sg::staging_binding_group_handle> try_create_staging_binding_group(
        sg::binding_group_layout_handle,
        sg::lifetime_scope) override
    {
        return cc::error("webgpu has no binding arrays, so it has no staging binding groups "
                         "(ctx.supports(sg::feature::binding_arrays) is false)");
    }

    sg::submission_token submit_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        return submit_webgpu_command_list(
            std::unique_ptr<webgpu_command_list>(static_cast<webgpu_command_list*>(cmd.release())));
    }
    void drop_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        drop_webgpu_command_list(std::unique_ptr<webgpu_command_list>(static_cast<webgpu_command_list*>(cmd.release())));
    }

    // Async and streaming transfers; see webgpu_stream_system.
    void async_upload_bytes_to_buffer(sg::raw_buffer_handle buffer, cc::pinned_data<byte const> data, isize offset) override
    {
        _streams.upload_buffer(cc::move(buffer), cc::move(data), offset);
    }
    void async_upload_bytes_to_texture(sg::raw_texture_handle texture,
                                       cc::pinned_data<byte const> data,
                                       sg::subresource_index const& subresource,
                                       sg::texture_region const& region) override
    {
        _streams.upload_texture(cc::move(texture), cc::move(data), subresource, region);
    }
    [[nodiscard]] sg::bytes_future async_download_bytes_from_buffer(sg::raw_buffer_handle buffer,
                                                                    isize offset,
                                                                    isize size_in_bytes) override
    {
        return _streams.download_buffer(cc::move(buffer), offset, size_in_bytes);
    }
    [[nodiscard]] sg::bytes_future async_download_bytes_from_texture(sg::raw_texture_handle texture,
                                                                     sg::subresource_index const& subresource,
                                                                     sg::texture_region const& region) override
    {
        return _streams.download_texture(cc::move(texture), subresource, region);
    }
    [[nodiscard]] sg::stream_upload_handle stream_bytes_to_buffer(sg::raw_buffer_handle buffer,
                                                                  cc::pinned_data<byte const> data,
                                                                  isize offset,
                                                                  sg::stream_scope) override
    {
        return _streams.stream_to_buffer(cc::move(buffer), sg::make_pinned_stream_source(cc::move(data)), offset);
    }
    [[nodiscard]] sg::stream_upload_handle stream_bytes_to_texture(sg::raw_texture_handle texture,
                                                                   cc::pinned_data<byte const> data,
                                                                   sg::subresource_index const& subresource,
                                                                   sg::texture_region const& region,
                                                                   sg::stream_scope) override
    {
        return _streams.stream_to_texture(cc::move(texture), sg::make_pinned_stream_source(cc::move(data)), subresource,
                                          region);
    }
    [[nodiscard]] sg::stream_upload_handle stream_source_to_buffer(sg::raw_buffer_handle buffer,
                                                                   std::unique_ptr<sg::stream_source> source,
                                                                   isize offset,
                                                                   sg::stream_scope) override
    {
        return _streams.stream_to_buffer(cc::move(buffer), cc::move(source), offset);
    }
    [[nodiscard]] sg::stream_upload_handle stream_source_to_texture(sg::raw_texture_handle texture,
                                                                    std::unique_ptr<sg::stream_source> source,
                                                                    sg::subresource_index const& subresource,
                                                                    sg::texture_region const& region,
                                                                    sg::stream_scope) override
    {
        return _streams.stream_to_texture(cc::move(texture), cc::move(source), subresource, region);
    }
    [[nodiscard]] sg::stream_download_handle stream_bytes_from_buffer(sg::raw_buffer_handle buffer,
                                                                      isize offset,
                                                                      isize size_in_bytes,
                                                                      sg::stream_scope) override
    {
        return _streams.stream_from_buffer(cc::move(buffer), sg::stream_sink{}, offset, size_in_bytes);
    }
    [[nodiscard]] sg::stream_download_handle stream_bytes_from_texture(sg::raw_texture_handle texture,
                                                                       sg::subresource_index const& subresource,
                                                                       sg::texture_region const& region,
                                                                       sg::stream_scope) override
    {
        return _streams.stream_from_texture(cc::move(texture), sg::stream_sink{}, subresource, region);
    }
    [[nodiscard]] sg::stream_download_handle stream_to_sink_from_buffer(sg::raw_buffer_handle buffer,
                                                                        sg::stream_sink sink,
                                                                        isize offset,
                                                                        isize size_in_bytes,
                                                                        sg::stream_scope) override
    {
        return _streams.stream_from_buffer(cc::move(buffer), cc::move(sink), offset, size_in_bytes);
    }
    [[nodiscard]] sg::stream_download_handle stream_to_sink_from_texture(sg::raw_texture_handle texture,
                                                                         sg::stream_sink sink,
                                                                         sg::subresource_index const& subresource,
                                                                         sg::texture_region const& region,
                                                                         sg::stream_scope) override
    {
        return _streams.stream_from_texture(cc::move(texture), cc::move(sink), subresource, region);
    }

    void set_inline_upload_budget(isize bytes) override { _upload_ring.set_budget(bytes); }

    /// WebGPU transitions nothing sg can see, so every layout is `general` and there is nothing to settle.
    [[nodiscard]] sg::texture_layout async_ready_layout(sg::async_direction) const override
    {
        return sg::texture_layout::general;
    }
    [[nodiscard]] sg::texture_layout current_texture_layout(sg::raw_texture_handle const&,
                                                            sg::subresource_range const&) const override
    {
        return sg::texture_layout::general;
    }

    // Epochs — bodies in webgpu_epoch.cc.

    [[nodiscard]] sg::epoch current_epoch() const override { return _current_epoch; }
    [[nodiscard]] sg::epoch completed_epoch() const override { return sg::epoch(_epochs.completed_epoch); }
    void advance_epoch() override;
    [[nodiscard]] int in_flight_epoch_count() override { return int(_epochs.in_flight.size()); }
    void retire_completed_epochs() override;
    [[nodiscard]] bool is_submission_complete(sg::submission_token token) const override;
    [[nodiscard]] bool are_transfers_drained() const override { return _readbacks.is_idle() && _streams.is_idle(); }
    [[nodiscard]] sg::submission_token last_issued_submission() override;

    /// Completion arrives through the work-done callbacks every submit registers, so there is nothing to arm.
    void arm_completion_signal(u64, u64) override {}

    // Unreachable: they would wait, and nothing reaches them on a never-block context.
    void wait_for_epoch(sg::epoch) override { CC_UNREACHABLE("webgpu cannot wait for an epoch"); }
    void wait_for_next_inflight_epoch() override { CC_UNREACHABLE("webgpu cannot wait for an epoch"); }
    void block_until_submissions_complete() override { CC_UNREACHABLE("webgpu cannot block"); }
    void block_until_transfers_drained() override { CC_UNREACHABLE("webgpu cannot block"); }

    /// Stages a released object for the retire of the open epoch.
    void schedule_deferred_deletion(webgpu_expiring_resource expiring);

    /// Registers a transient resource for expiry at the next advance, and hands it back.
    [[nodiscard]] std::shared_ptr<webgpu_buffer> register_if_transient(std::shared_ptr<webgpu_buffer> buffer,
                                                                       sg::lifetime_scope scope);
    [[nodiscard]] std::shared_ptr<webgpu_texture> register_if_transient(std::shared_ptr<webgpu_texture> texture,
                                                                        sg::lifetime_scope scope);

    /// Asks the queue to call back once everything submitted so far has run, raising the completed submission to `submission` and the completed epoch to `epoch`, where nonzero.
    void notify_when_queue_done(u64 submission, u64 epoch);

    /// Called from a work-done callback.
    void on_queue_done(u64 submission, u64 epoch);

    void shutdown() override;

    // Subsystems.
    webgpu_upload_ring _upload_ring;
    webgpu_readback_pool _readbacks;
    webgpu_constant_pages _constant_pages;
    webgpu_sampler_cache _samplers;
    webgpu_stream_system _streams;
    webgpu_query_system _queries;

    /// The optional device features creation was granted.
    struct granted_features
    {
        bool timestamps = false;
        bool readwrite_storage_formats = false; ///< texture-formats-tier2
        bool float32_filtering = false;         ///< float32-filterable
        bool extended_storage_formats = false;  ///< texture-formats-tier1 and bgra8unorm-storage
    };

    // Set once at creation.
    void set_limits(isize uniform_offset_alignment, granted_features const& features);

private:
    [[nodiscard]] static webgpu_binding_group_layout_handle as_webgpu_layout(sg::binding_group_layout_handle const& layout);

    std::shared_ptr<webgpu_callback_anchor> _anchor;
    wgpu_instance _instance;
    wgpu_adapter _adapter;
    wgpu_device _device;
    wgpu_queue _queue;
    webgpu_config _config;
    isize _uniform_offset_alignment = 256;
    bool _readwrite_storage_formats = false; // texture-formats-tier2 was granted
    bool _float32_filtering = false;
    bool _extended_storage_formats = false;

    sg::epoch _current_epoch = sg::epoch::first;
    u64 _next_submission = u64(sg::submission_token::first);
    int _open_command_lists = 0;
    webgpu_epoch_state _epochs;

    cc::vector<std::weak_ptr<sg::raw_buffer const>> _transient_buffers;
    cc::vector<std::weak_ptr<sg::raw_texture const>> _transient_textures;
};

namespace sg
{
/// Creates a context on the WebGPU backend: requests an adapter and a device, and settles once both arrived.
///
/// The device is requested with WebGPU's default limits, which is what every portable sg caller already sizes against.
/// `timestamp-query`, `texture-compression-bc`, `depth32float-stencil8`, `depth-clip-control` and `texture-formats-tier1` / `-tier2` are requested where the adapter offers them.
/// Callable from any thread: off main, the request moves to the main thread first, which is then the context's device thread.
/// Fails, as the node's error, where there is no WebGPU at all or no adapter.
[[nodiscard]] cc::shared_async<context_handle> request_webgpu_context(backend::webgpu::webgpu_config const& config = {});

/// Wraps a device someone else already requested; called on main, where a main_thread context lives.
/// Its errors reach take_pending_errors only where the device was requested with this backend's callbacks, which `request_webgpu_context` installs.
/// A foreign device's uncaptured errors stay with whoever requested it.
[[nodiscard]] cc::result<context_handle> create_webgpu_context(WGPUDevice device,
                                                               backend::webgpu::webgpu_config const& config = {});
} // namespace sg
