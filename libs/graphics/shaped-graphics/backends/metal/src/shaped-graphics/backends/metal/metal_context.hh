#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/error/result.hh>
#include <clean-core/function/unique_function.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_binding_group.hh>
#include <shaped-graphics/backends/metal/metal_binding_layout.hh>
#include <shaped-graphics/backends/metal/metal_buffer.hh>
#include <shaped-graphics/backends/metal/metal_command_list.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/backends/metal/metal_compute_pipeline.hh>
#include <shaped-graphics/backends/metal/metal_epoch.hh>
#include <shaped-graphics/backends/metal/metal_feedback.hh>
#include <shaped-graphics/backends/metal/metal_memory_heap.hh>
#include <shaped-graphics/backends/metal/metal_raster_pipeline.hh>
#include <shaped-graphics/backends/metal/metal_residency.hh>
#include <shaped-graphics/backends/metal/metal_sampler_cache.hh>
#include <shaped-graphics/backends/metal/metal_staging_ring.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>
#include <shaped-graphics/backends/metal/metal_texture_view_cache.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/binding/compiled_shader.hh> // sg::shader_format, which k_accepted_shader_formats names
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>

/// Per-backend creation config for the Metal context.
///
/// Empty so far, and the notable absence is a validation knob.
/// dx12 and vulkan each take one, and Metal has no equivalent to switch on: its API and shader validation layers are
/// enabled by the `MTL_DEBUG_LAYER` / `MTL_SHADER_VALIDATION` environment variables, read before any code of ours runs,
/// and they log and abort rather than calling back.
/// Settling what a listener here could even be told is the next piece of work on this backend — see
/// libs/graphics/shaped-graphics/backends/metal/readme.md.
struct sg::backend::metal::metal_config
{
    /// Capacity of the staging ring behind cmd.upload, in bytes.
    /// One epoch's inline uploads must fit, since the ring is only reclaimed when an epoch retires.
    /// Matches the other two backends' default.
    isize upload_ring_bytes = 16 * 1024 * 1024;

    /// Capacity of the readback ring behind cmd.download, in bytes.
    isize download_ring_bytes = 16 * 1024 * 1024;
};

/// Metal implementation of sg::context, on Metal 4.
///
/// The bodies live in the sibling metal_*.cc files; metal_context.create.cc owns bring-up.
/// Only the device, the queue and the epoch timelines are real so far — every resource and recording seam asserts.
/// See libs/graphics/shaped-graphics/docs/writing-a-backend.md for the milestone order.
class sg::backend::metal::metal_context final : public sg::context
{
    // metal consumes compiled Metal libraries only.
    static constexpr sg::shader_format k_accepted_shader_formats[] = {sg::shader_format::metal_lib};

public:
    /// Takes ownership of every argument; `create_metal_context` is what assembles them.
    ///
    /// The two events are handed over rather than an assembled epoch system, because that system owns a mutex and is
    /// therefore neither copyable nor movable — so it is built in place here.
    metal_context(MTL::Device* device,
                  MTL4::CommandQueue* queue,
                  MTL4::Compiler* compiler,
                  MTL::SharedEvent* epoch_event,
                  MTL::SharedEvent* submission_event);
    ~metal_context() override;

    // create_metal_context fills this in once it has picked a device.
    using sg::context::set_adapter_info;

    [[nodiscard]] MTL::Device* device() const { return _device; }
    [[nodiscard]] MTL4::CommandQueue* queue() const { return _queue; }
    [[nodiscard]] metal_epoch_system& epochs() { return _epochs; }

    /// Hands each open command list its index into every resource's concurrent access tracking.
    [[nodiscard]] sg::command_list_slot_allocator& slots() { return _slots; }

    /// Everything this context's GPU work may touch; MTL4 has no useResource, so a resource outside this is not there.
    [[nodiscard]] metal_residency_set& residency() { return _residency; }

    /// MTLSamplerStates for bound sampler values, shared context-wide.
    [[nodiscard]] metal_sampler_cache& samplers() { return _samplers; }

    /// MTLTextures for bound texture views, shared context-wide and keyed on sg view identity.
    [[nodiscard]] metal_texture_view_cache& texture_views() { return _texture_views; }

    /// The MTL4 compiler every pipeline is built through.
    /// One per context: MTL4 makes compilation an explicit object where Metal 3 hid it behind the device.
    [[nodiscard]] MTL4::Compiler* compiler() const { return _compiler; }

    /// The rings inline transfers stage through, guarded because a list may record on any thread.
    [[nodiscard]] cc::mutex<metal_staging_ring>& upload_ring() { return _upload_ring; }
    [[nodiscard]] cc::mutex<metal_staging_ring>& download_ring() { return _download_ring; }

    /// Metal has every stage sg models except the two geometry-pipeline ones, which it has never had.
    [[nodiscard]] bool supports(sg::feature f) const override;

    /// The backend-typed buffer create, which the sg::context virtual forwards to.
    [[nodiscard]] cc::result<metal_buffer_handle> create_metal_buffer(isize size_in_bytes,
                                                                      sg::buffer_usages usage,
                                                                      sg::allocation_info const& alloc);

    [[nodiscard]] cc::result<metal_texture_handle> create_metal_texture(sg::texture_description const& desc,
                                                                        sg::allocation_info const& alloc);

    /// The backend-typed heap create, which the sg::context virtual forwards to.
    [[nodiscard]] cc::result<metal_memory_heap_handle> create_metal_memory_heap(isize size_in_bytes);

    // The bind path's backend-typed creates; the sg::context virtuals forward to these.
    [[nodiscard]] cc::result<metal_binding_group_layout_handle> create_metal_binding_group_layout(
        cc::span<sg::binding const> bindings,
        cc::span<sg::named_sampler const> static_samplers,
        sg::lifetime_scope scope);
    [[nodiscard]] cc::result<metal_pipeline_layout_handle> create_metal_pipeline_layout(
        sg::pipeline_layout_description const& desc,
        sg::lifetime_scope scope);
    [[nodiscard]] cc::result<swapchain_handle> create_metal_swapchain(sg::swapchain_description const& desc);
    [[nodiscard]] cc::result<metal_raster_pipeline_handle> create_metal_raster_pipeline(
        sg::raster_pipeline_description const& desc,
        sg::lifetime_scope scope);
    [[nodiscard]] cc::result<metal_compute_pipeline_handle> create_metal_compute_pipeline(
        sg::compute_pipeline_description const& desc,
        sg::lifetime_scope scope);
    [[nodiscard]] cc::result<staging_binding_group_handle> create_metal_staging_binding_group(
        sg::binding_group_layout_handle layout,
        sg::lifetime_scope scope);
    [[nodiscard]] cc::result<metal_binding_group_handle> create_metal_binding_group(
        sg::binding_group_layout_handle const& layout,
        cc::span<sg::named_view const> views,
        cc::span<sg::named_sampler const> samplers,
        sg::lifetime_scope scope);

    /// Allocates the residency set and the two staging rings.
    /// Called once by create_metal_context, before the context is handed out.
    void create_staging_rings(isize upload_bytes, isize download_bytes);

    /// Move every resource `list` touched from its per-list state into the state the next list synchronizes against.
    void finalize_touched_buffers(metal_command_list& list);

    /// Publish one commit's failure on the deferred error channel; `metal_feedback_sink` is the only caller.
    ///
    /// Public because the sink is a separate object by necessity — it has to outlive this context — and a friend
    /// declaration would buy nothing a backend's readability-over-encapsulation stance wants.
    void report_feedback_error(sg::device_error_kind kind, cc::string_view message);

    // The sg::context surface.
    // Everything not listed here is still a stub in metal_context.cc.
    [[nodiscard]] sg::epoch current_epoch() const override { return _epochs.current(); }
    [[nodiscard]] sg::epoch completed_epoch() const override { return _epochs.completed(); }
    void advance_epoch() override;
    [[nodiscard]] int in_flight_epoch_count() override { return _epochs.in_flight_count(); }
    [[nodiscard]] bool is_submission_complete(sg::submission_token token) const override
    {
        return _epochs.is_submission_complete(token);
    }

    sg::submission_token submit_command_list(std::unique_ptr<sg::command_list> cmd) override;
    void drop_command_list(std::unique_ptr<sg::command_list> cmd) override;

    void shutdown() override;

private:
    // Runs shutdown() before the base dtor asserts it, and swallows what shutdown() throws — a destructor reached while
    // a device-loss exception is unwinding would otherwise call std::terminate.
    void shutdown_no_throw() noexcept;

    [[nodiscard]] cc::result<std::unique_ptr<sg::command_list>> try_create_command_list() override;

    void wait_for_epoch(sg::epoch e) override { _epochs.wait_for(e); }
    void wait_for_next_inflight_epoch() override { _epochs.wait_for_next_inflight(); }
    void retire_completed_epochs() override { _epochs.retire_completed(); }
    void block_until_submissions_complete() override { _epochs.block_until_submissions_complete(); }
    /// Wait until every download's copy-out has run.
    ///
    /// Draining the GPU is not enough on its own: a commit's feedback handler runs on a dispatch queue after the GPU
    /// finished, so a caller that only waited on the epoch fence could observe an unsettled future.
    void block_until_transfers_drained() override;

    [[nodiscard]] cc::result<swapchain_handle> try_create_swapchain(swapchain_description const& desc) override;

    [[nodiscard]] texture_layout async_ready_layout(async_direction direction) const override;
    [[nodiscard]] texture_layout current_texture_layout(raw_texture_handle const& texture,
                                                        subresource_range const& range) const override;

    void async_upload_bytes_to_buffer(raw_buffer_handle buffer,
                                      cc::pinned_data<byte const> data,
                                      isize offset_in_bytes) override;
    void async_upload_bytes_to_texture(raw_texture_handle texture,
                                       cc::pinned_data<byte const> data,
                                       subresource_index const& subresource,
                                       texture_region const& region) override;
    [[nodiscard]] bytes_future async_download_bytes_from_buffer(raw_buffer_handle buffer,
                                                                isize offset_in_bytes,
                                                                isize size_in_bytes) override;
    [[nodiscard]] bytes_future async_download_bytes_from_texture(raw_texture_handle texture,
                                                                 subresource_index const& subresource,
                                                                 texture_region const& region) override;

    [[nodiscard]] stream_upload_handle stream_bytes_to_buffer(raw_buffer_handle buffer,
                                                              cc::pinned_data<byte const> data,
                                                              isize offset_in_bytes,
                                                              stream_scope scope) override;
    [[nodiscard]] stream_upload_handle stream_bytes_to_texture(raw_texture_handle texture,
                                                               cc::pinned_data<byte const> data,
                                                               subresource_index const& subresource,
                                                               texture_region const& region,
                                                               stream_scope scope) override;
    [[nodiscard]] stream_upload_handle stream_source_to_buffer(raw_buffer_handle buffer,
                                                               std::unique_ptr<stream_source> source,
                                                               isize offset_in_bytes,
                                                               stream_scope scope) override;
    [[nodiscard]] stream_upload_handle stream_source_to_texture(raw_texture_handle texture,
                                                                std::unique_ptr<stream_source> source,
                                                                subresource_index const& subresource,
                                                                texture_region const& region,
                                                                stream_scope scope) override;
    [[nodiscard]] stream_download_handle stream_bytes_from_buffer(raw_buffer_handle buffer,
                                                                  isize offset_in_bytes,
                                                                  isize size_in_bytes,
                                                                  stream_scope scope) override;
    [[nodiscard]] stream_download_handle stream_bytes_from_texture(raw_texture_handle texture,
                                                                   subresource_index const& subresource,
                                                                   texture_region const& region,
                                                                   stream_scope scope) override;
    [[nodiscard]] stream_download_handle stream_to_sink_from_buffer(raw_buffer_handle buffer,
                                                                    stream_sink sink,
                                                                    isize offset_in_bytes,
                                                                    isize size_in_bytes,
                                                                    stream_scope scope) override;
    [[nodiscard]] stream_download_handle stream_to_sink_from_texture(raw_texture_handle texture,
                                                                     stream_sink sink,
                                                                     subresource_index const& subresource,
                                                                     texture_region const& region,
                                                                     stream_scope scope) override;

    [[nodiscard]] cc::result<raw_buffer_handle> try_create_raw_buffer(isize size_in_bytes,
                                                                      buffer_usages usage,
                                                                      allocation_info const& alloc) override;
    [[nodiscard]] cc::result<raw_texture_handle> try_create_raw_texture(texture_description const& desc,
                                                                        allocation_info const& alloc) override;
    [[nodiscard]] cc::result<memory_heap_handle> try_create_memory_heap(isize size_in_bytes) override;

    [[nodiscard]] cc::result<binding_group_layout_handle> try_create_binding_group_layout(
        cc::span<binding const> bindings,
        cc::span<named_sampler const> static_samplers,
        lifetime_scope scope) override;
    [[nodiscard]] cc::result<pipeline_layout_handle> try_create_pipeline_layout(pipeline_layout_description const& desc,
                                                                                lifetime_scope scope) override;
    [[nodiscard]] cc::result<compute_pipeline_handle> try_create_compute_pipeline(compute_pipeline_description const& desc,
                                                                                  lifetime_scope scope) override;
    [[nodiscard]] cc::result<raster_pipeline_handle> try_create_raster_pipeline(raster_pipeline_description const& desc,
                                                                                lifetime_scope scope) override;
    [[nodiscard]] cc::result<raytracing_pipeline_handle> try_create_raytracing_pipeline(
        raytracing_pipeline_description const& desc,
        lifetime_scope scope) override;
    [[nodiscard]] cc::result<raytracing_shader_table_handle> try_create_raytracing_shader_table(
        raytracing_shader_table_description const& desc,
        lifetime_scope scope) override;
    [[nodiscard]] cc::result<binding_group_handle> try_create_binding_group(binding_group_layout_handle layout,
                                                                            cc::span<named_view const> views,
                                                                            cc::span<named_sampler const> samplers,
                                                                            lifetime_scope scope) override;
    [[nodiscard]] cc::result<staging_binding_group_handle> try_create_staging_binding_group(
        binding_group_layout_handle layout,
        lifetime_scope scope) override;

    MTL::Device* _device = nullptr;
    MTL4::CommandQueue* _queue = nullptr;
    metal_epoch_system _epochs;

    /// Shared with every commit-feedback handler still in flight; detached at shutdown.
    std::shared_ptr<metal_feedback_sink> _feedback;

    sg::command_list_slot_allocator _slots;
    metal_residency_set _residency;
    metal_sampler_cache _samplers;
    metal_texture_view_cache _texture_views;
    MTL4::Compiler* _compiler = nullptr;
    cc::mutex<metal_staging_ring> _upload_ring;
    cc::mutex<metal_staging_ring> _download_ring;

    /// Download copy-outs committed but not yet run, so block_until_transfers_drained knows when it is done.
    std::atomic<int> _pending_downloads = 0;

    /// Transient resources created in the open epoch, expired when it closes.
    ///
    /// Weak, because a caller may well have dropped its handle already and nothing here should keep the resource
    /// alive — the point is only to reach the ones still held, so a handle kept past its epoch reports itself expired
    /// rather than naming storage the bump heap has already handed to somebody else.
    cc::mutex<cc::vector<std::weak_ptr<sg::raw_buffer const>>> _transient_expiring;
    cc::mutex<cc::vector<std::weak_ptr<sg::raw_texture const>>> _transient_expiring_textures;
};

namespace sg
{
/// Creates a context on the Metal backend.
///
/// Fails, naming what is missing, on anything below the backend's floor: macOS / iOS 26 for the Metal 4 API, and a
/// device in the Metal 4 GPU family — Apple silicon M1 and later, A14 and later.
/// A host with no Metal device at all fails the same way, which is what lets a test SKIP rather than pass silently.
[[nodiscard]] cc::result<context_handle> create_metal_context(backend::metal::metal_config const& config = {});
} // namespace sg
