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
#include <shaped-graphics/backends/metal/metal_query.hh>
#include <shaped-graphics/backends/metal/metal_raster_pipeline.hh>
#include <shaped-graphics/backends/metal/metal_residency.hh>
#include <shaped-graphics/backends/metal/metal_sampler_cache.hh>
#include <shaped-graphics/backends/metal/metal_staging_ring.hh>
#include <shaped-graphics/backends/metal/metal_stream.hh>
#include <shaped-graphics/backends/metal/metal_texture.hh>
#include <shaped-graphics/backends/metal/metal_texture_view_cache.hh>
#include <shaped-graphics/backends/metal/metal_transfer.hh>
#include <shaped-graphics/barrier/command_list_slot.hh>
#include <shaped-graphics/binding/compiled_shader.hh> // sg::shader_format, which k_accepted_shader_formats names
#include <shaped-graphics/context/context.hh>
#include <shaped-graphics/context/impl/completion_waiter.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>
#include <condition_variable>
#include <mutex>

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

    /// The off-frame transfer queue and its ordering timeline.
    [[nodiscard]] metal_transfer_system& transfers() { return _transfers; }

    [[nodiscard]] metal_stream_system& streams() { return _streams; }

    /// The GPU-timestamp tier behind cmd.query, and what says whether this device has one.
    [[nodiscard]] metal_query_system& queries() { return _queries; }

    /// MTLSamplerStates for bound sampler values, shared context-wide.
    [[nodiscard]] metal_sampler_cache& samplers() { return _samplers; }

    /// MTLTextures for bound texture views, shared context-wide and keyed on sg view identity.
    [[nodiscard]] metal_texture_view_cache& texture_views() { return _texture_views; }

    /// The MTL4 compiler every pipeline is built through.
    /// One per context: MTL4 makes compilation an explicit object where Metal 3 hid it behind the device.
    [[nodiscard]] MTL4::Compiler* compiler() const { return _compiler; }

    /// The rings inline transfers stage through, guarded because a list may record on any thread.
    [[nodiscard]] metal_staging_ring& upload_ring() { return _upload_ring; }
    [[nodiscard]] metal_staging_ring& download_ring() { return _download_ring; }

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
    [[nodiscard]] cc::result<sg::raytracing_pipeline_handle> create_metal_raytracing_pipeline(
        sg::raytracing_pipeline_description const& desc,
        sg::lifetime_scope scope);
    [[nodiscard]] cc::result<sg::raytracing_shader_table_handle> create_metal_raytracing_shader_table(
        sg::raytracing_shader_table_description const& desc,
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
    [[nodiscard]] cc::result<cc::unit> create_systems(isize upload_bytes, isize download_bytes);

    /// Resize an inline staging ring, which metal does by replacing it.
    /// The drain is what makes that safe: a ring's bytes are named by copies recorded into command buffers, so the old
    /// storage cannot go until everything holding a reservation has run.
    void set_inline_upload_budget(isize bytes) override;
    void set_inline_download_budget(isize bytes) override;

    /// The half both budget setters share.
    void resize_ring(metal_staging_ring& ring, isize bytes, cc::string_view label, cc::string_view kind);


    /// The transfer-timeline value `list` must wait for before it may run, or 0 when none of its resources has a
    /// transfer in flight.
    [[nodiscard]] pending_transfers highest_pending_transfer(metal_command_list& list) const;

    /// Move every resource `list` touched from its per-list state into the state the next list synchronizes against.
    void finalize_touched_buffers(metal_command_list& list);

    /// The highest submission token any resource this list touches was last named by, or 0 for none.
    /// Read before `stamp_touched_resources` raises them, and it is what this submit waits on.
    [[nodiscard]] u64 highest_prior_submission(metal_command_list& list) const;

    /// Record `token` on every resource `list` touched, so a later off-frame transfer defers behind this list.
    /// The reverse of `highest_pending_transfer`, and the other half of the sync between the two queues.
    void stamp_touched_resources(metal_command_list& list, sg::submission_token token);

    /// Make `list` wait for every streaming transfer still filling a resource it touched, warning once per stream.
    ///
    /// The wait is what makes a stream as safe as an async transfer rather than a documented data race, and it is
    /// also a stall — so the first list it costs is told, unless `promote_to_async` already declared it intended.
    void wait_for_streams(metal_command_list& list);

    /// The detachable end every commit feedback handler routes through.
    /// The transfer and stream handlers capture it to report a drain reaching zero, the same way the submit handler
    /// already captures it to report an error.
    [[nodiscard]] std::shared_ptr<metal_feedback_sink> const& feedback_sink() const { return _feedback; }

    /// Raise the GPU side of the completion generation; `metal_feedback_sink` is the only caller.
    void notify_completion_signal();

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
    [[nodiscard]] bool are_transfers_drained() const override;
    [[nodiscard]] submission_token last_issued_submission() override;

    /// Metal cannot wait on several timelines at once, so the waiter parks on a condition every source raises.
    /// `arm_completion_signal` builds one on first use, the way dx12 and vulkan do.
    void arm_completion_signal(u64 submission, u64 epoch) override;
    void park_for_completion_signal(u64 submission, u64 epoch, u64 wake_generation);
    void wake_completion_signal(u64 generation);

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

    void set_stream_upload_ratio(float ratio) override;
    void set_stream_download_ratio(float ratio) override;
    void set_stream_upload_aging(float per_second) override;
    void set_stream_download_aging(float per_second) override;

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
    [[nodiscard]] cc::result<binding_group_handle> try_create_binding_group(binding_group_layout_handle layout,
                                                                            cc::span<slotted_view const> views,
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
    metal_transfer_system _transfers;
    metal_stream_system _streams;
    metal_query_system _queries;
    metal_texture_view_cache _texture_views;
    MTL4::Compiler* _compiler = nullptr;
    /// Serializes finalize, commit and signal on the direct queue, so a token's order is the order its work is
    /// signalled in.
    ///
    /// The queue itself is free-threaded, and that is the problem: two threads may claim tokens 5 and 6 and reach the
    /// commit in the other order, which either releases a waiter on 5 before list 5 has run, or drives the shared event
    /// backwards from 6 to 5 and breaks `is_submission_complete`.
    /// Finalize belongs inside the same section for a second reason — finalize order must equal execute order, which is
    /// what makes a resource's `current` mean "after everything submitted so far".
    /// dx12 and vulkan both hold one lock across the same three steps.
    cc::mutex<int> _submission;

    metal_staging_ring _upload_ring;
    metal_staging_ring _download_ring;

    /// What a completion-signal waiter parks on, and what the GPU's notification handlers wake it through.
    ///
    /// Parks on the GPU timelines and settles what they reach; built on the first arm.
    std::unique_ptr<sg::impl::completion_waiter> _completion_waiter;

    /// **A real mutex and condition even with SC_THREADS off.**
    /// `MTL::SharedEvent::notifyListener` runs its block on a dispatch queue Apple owns, which that flag does not
    /// reach — the same hole `callback_mutex` exists for.
    struct completion_signal
    {
        std::mutex mutex;
        std::condition_variable condition;
        MTL::SharedEventListener* listener = nullptr;

        /// The value each timeline is currently armed at.
        /// Re-armed whenever the target *differs*, not only when it grows: the portable layer wakes this waiter
        /// precisely when a new target is lower than the armed one, so `>=` would leave that target unarmed and wait
        /// for the higher one instead.
        /// The waiter is the single caller, which is what lets these live outside the mutex.
        u64 armed_submission = 0;
        u64 armed_epoch = 0;

        /// Raised once per GPU notification, and the value the last wait returned at.
        /// The pair is what makes each notification release exactly one wait: comparing against the host's counter
        /// instead leaves the predicate true forever after the first GPU signal, which is a spin rather than a wait.
        u64 gpu_generation = 0;
        u64 consumed_gpu_generation = 0;

        /// The highest generation `wake_completion_signal` has been given.
        /// Compared against the caller's own `wake_generation`, which is the counter it read before parking.
        u64 host_generation = 0;
    };

    mutable completion_signal _completion;

    /// Download copy-outs committed but not yet run, so block_until_transfers_drained knows when it is done.
    ///
    /// `std::atomic` rather than `cc::atomic`, for the reason `callback_mutex` exists: a commit handler decrements
    /// this from a dispatch queue Apple owns, and `cc::atomic` is a plain value once `SC_THREADS` is off.
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
