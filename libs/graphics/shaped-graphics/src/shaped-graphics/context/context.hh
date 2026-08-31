#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <clean-core/thread/thread_pump.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/context/adapter_info.hh>
#include <shaped-graphics/context/cached.hh>
#include <shaped-graphics/context/download.hh>
#include <shaped-graphics/context/gpu_metrics.hh>
#include <shaped-graphics/context/persistent.hh>
#include <shaped-graphics/context/transient.hh>
#include <shaped-graphics/context/uncached.hh>
#include <shaped-graphics/context/upload.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/present/swapchain.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-graphics/routine/routine_registry.hh>
#include <shaped-graphics/transfer/stream.hh>
#include <shaped-graphics/transfer/stream_sink.hh>
#include <shaped-graphics/types.hh>

/// Mutable entry point to a graphics backend: the factory for command lists and GPU resources.
/// Abstract — a backend subclasses it (e.g. sg::backend::vulkan::vulkan_context), and you obtain one from that backend's sg::create_<backend>_context(config).
/// Must outlive every command list and resource it creates.
/// A backend's destructor runs shutdown() for you; call it yourself only to release the device early.
class sg::context
{
public:
    virtual ~context();

    /// The backend kind driving this context — a coarse tag, not the concrete type.
    [[nodiscard]] backend_kind backend() const { return _backend; }

    /// Bytecode formats this context can build pipelines from, most-preferred first.
    /// Never empty; a shader in any other format is rejected at pipeline creation.
    [[nodiscard]] cc::span<shader_format const> accepted_shader_formats() const { return _accepted_shader_formats; }

    /// Whether this context can build pipelines from `format`.
    [[nodiscard]] bool accepts_shader_format(shader_format format) const;

    /// Whether `ctx.create_swapchain` can be given a `headless_extent` on this context.
    ///
    /// A build-and-device fact rather than a preference, and it differs by backend for a real reason: vulkan needs
    /// VK_EXT_headless_surface plus VK_KHR_swapchain, while dx12 emulates the whole thing with ordinary render-target
    /// textures and therefore always can.
    /// Defaults to false, so a backend that has not implemented headless present reports it rather than failing at
    /// creation — which is what lets a test skip cleanly instead of asserting.
    [[nodiscard]] virtual bool supports_headless_present() const { return false; }

    /// The threading guarantees this backend provides (see libs/graphics/shaped-graphics/docs/concepts/threading.md).
    [[nodiscard]] thread_model threading() const { return _thread_model; }

    /// Which GPU this context is running on, fixed at creation.
    /// Fields a backend cannot report are left at their defaults, so a caller reads "unknown" and never a wrong answer.
    [[nodiscard]] adapter_info const& adapter() const { return _adapter; }

    // GPU metrics.
    //
    // Non-pure with a refusing default, so a backend that cannot answer needs no code at all and a caller gets a clean
    // error rather than a fabricated zero.

    /// What this process may use of the GPU's memory right now, and what it is using.
    ///
    /// Portable in principle and available on both shipping backends: DXGI reports it directly, and Vulkan does where
    /// VK_EXT_memory_budget is present.
    /// The card's own size is `adapter().dedicated_video_memory_bytes`, and the two are different scales — see there.
    [[nodiscard]] virtual cc::result<gpu_memory_usage> query_gpu_memory() const;

    /// Monotone busy time per GPU engine class, for sg::gpu_load_sampler to difference.
    ///
    /// **Neither D3D12 nor Vulkan exposes utilization**, so this comes from the OS instead: the GPU Engine performance
    /// counters on Windows, `raw:/sys/class/drm/*/device/gpu_busy_percent` on Linux where the driver provides one,
    /// IOKit on macOS.
    /// Only the Windows path exists today; everywhere else this refuses rather than guessing.
    ///
    /// A caller almost always wants sg::gpu_load_sampler rather than this — the counters are published because a rate
    /// has thrown the seconds away and somebody always wants them back.
    [[nodiscard]] virtual cc::result<gpu_counters> read_gpu_counters() const;

    /// Whether the GPU device has been lost — driver reset, TDR, removed adapter.
    /// Sticky once set: the context is unusable and must be torn down and recreated.
    /// Submit / advance / fence waits and the throwing create façades raise sg::device_lost_exception once this is true.
    /// Device loss surfaces on the `try_*` surface only as an ordinary, unclassified error, so a caller polling it must check this to break its retry loop.
    [[nodiscard]] bool is_device_lost() const { return _device_lost; }

    /// Backend-provided reason the device was lost; empty while the device is healthy.
    [[nodiscard]] cc::string_view device_loss_reason() const { return _device_loss_reason; }

    /// Long-lived GPU resources: `ctx.persistent.create_raw_buffer(...)`.
    context_persistent_scope persistent;

    /// Per-frame scratch, recycled when the epoch retires: `ctx.transient.create_raw_buffer(...)`.
    context_transient_scope transient;

    /// Async host→device streaming off the frame path: `ctx.upload.bytes_to_buffer(...)`.
    context_upload_scope upload;

    /// Async device→host readback off the frame path: `ctx.download.bytes_from_buffer(...)`.
    context_download_scope download;

    /// Bulk transfers that may take a while, with priority / progress / cancellation: `ctx.stream.bytes_to_buffer(...)`.
    /// The weaker sibling of upload / download: no automatic command-list synchronization, a handle instead.
    context_stream_scope stream;

    /// Raw, uncached layout / pipeline factory: `ctx.uncached.create_binding_group_layout(...)` — prefer `ctx.cached`.
    context_uncached_scope uncached;

    /// Deduplicated, async layout / pipeline cache: `ctx.cached.acquire_compute_pipeline(...)`.
    context_cached_scope cached;

    /// Per-context render-routine registry (see routine_registry / render_routine).
    /// Routines are reached by type through `sg::render_routine::acquire_exclusive(cmd)`, or `acquire(cmd)` to only read.
    /// Touch this to `prewarm<...>()` before opening a list, or to `evict<R>()` / `clear()` cached routine GPU state early.
    /// Cleared on shutdown.
    routine_registry routines;

    /// Opens a new command list, already recording.
    /// Single-use: submit or drop it exactly once, in the epoch it was opened in.
    /// Throws sg::device_lost_exception if the device has been lost; any other creation failure is an internal bug and aborts.
    [[nodiscard]] std::unique_ptr<command_list> create_command_list();

    /// Creates a swapchain that presents into the window named by `desc` (see swapchain_description).
    /// Throwing façade over try_create_swapchain — sg::device_lost_exception if the device was lost, sg::swapchain_creation_exception on a bad handle / format / DXGI error.
    [[nodiscard]] swapchain_handle create_swapchain(swapchain_description const& desc = {});

    /// The fallible core behind create_swapchain: returns a swapchain or a cc::error, never throws.
    /// A backend that cannot present (no windowing) returns an error.
    /// Device loss is marked but still surfaces here as an error — the throwing façade classifies it.
    [[nodiscard]] virtual cc::result<swapchain_handle> try_create_swapchain(swapchain_description const& desc = {}) = 0;

    /// Submits a command list for execution and consumes it, returning a token for its completion.
    virtual submission_token submit_command_list(std::unique_ptr<command_list> cmd) = 0;

    /// Submits `cmd` and presents `sc`'s acquired back buffer — the way to present.
    /// `cmd` must contain this frame's rendering into the back buffer acquired from `sc`.
    /// The back buffer's final transition to the present layout is folded into `cmd` rather than recorded on a separate list.
    submission_token submit_command_list_and_present(swapchain& sc, std::unique_ptr<command_list> cmd);

    /// Discards a command list unsubmitted and consumes it — the same as letting it go out of scope.
    virtual void drop_command_list(std::unique_ptr<command_list> cmd) = 0;

    // Epochs — frame-level GPU lifetime + CPU↔GPU sync.
    // See libs/graphics/shaped-graphics/docs/concepts/epochs.md.
public:
    /// The layout a texture must be in for an async or streaming transfer of `direction` to copy it without a barrier
    /// of its own — `general` on dx12, whose copy queue cannot run layout barriers at all, and a transfer layout on
    /// vulkan.
    /// `cmd.prepare_for_async` is what a caller writes; this is the seam it resolves through.
    [[nodiscard]] virtual texture_layout async_ready_layout(async_direction direction) const = 0;

    /// The layout `range` of `texture` is in as of the last submitted command list.
    /// A range spanning subresources in different layouts reports the first one, which is all a whole-subresource
    /// transfer needs.
    [[nodiscard]] virtual texture_layout current_texture_layout(raw_texture_handle const& texture,
                                                                subresource_range const& range) const = 0;

    /// Settle `range` of `texture` into the layout an async or streaming transfer of `direction` needs, and return the
    /// token of the submit that did it — nullopt when the texture was already there and nothing was submitted.
    ///
    /// A transfer queue cannot settle a layout for itself: a D3D12 copy queue runs no layout barriers at all, and a
    /// vulkan transfer queue that runs them puts a claim on a timeline the validation layer reads in submit-call
    /// order rather than in GPU order.
    /// So the direct queue does it, here, as a throwaway command list holding one transition.
    ///
    /// **It warns, once per texture**, because the caller could have avoided the submit entirely by recording
    /// `cmd.prepare_for_async` on a list they were already building.
    /// There is no opt-out on purpose: doing that is both the fix and the thing the warning asks for.
    [[nodiscard]] cc::optional<submission_token> prepare_texture_for_async(raw_texture_handle const& texture,
                                                                           subresource_range const& range,
                                                                           async_direction direction);

    /// The epoch new work is currently recorded into.
    [[nodiscard]] virtual epoch current_epoch() const = 0;

    /// The latest epoch whose GPU work has fully finished (its resources are reclaimable).
    [[nodiscard]] virtual epoch completed_epoch() const = 0;

    /// Closes the current epoch and opens the next, gating all its GPU work behind one fence value.
    /// Every command list opened this epoch must already be submitted or dropped.
    /// This epoch's garbage becomes reclaimable once that fence signals.
    ///
    /// `allowed_in_flight` throttles pipelining depth, and so bounds how far the CPU runs ahead of the GPU.
    /// nullopt never waits; 0 fully drains the GPU before returning; N keeps at most N prior epochs in flight.
    /// A windowed renderer typically passes its swapchain back-buffer count.
    virtual void advance_epoch(cc::optional<int> allowed_in_flight) = 0;

    /// Advance the epoch and block until the GPU is fully idle — equivalent to advance_epoch(0).
    /// Idle drains GPU work but not the readback actor.
    /// An inline-download future may still be undelivered right after; use wait_for(future) to be certain.
    virtual void advance_epoch_and_wait_for_idle() = 0;

    /// Reclaims everything owned by epochs the GPU has finished.
    /// Safe to call at any time and from any thread, but not concurrently with advance_epoch; also runs implicitly after the waits below.
    virtual void process_completed_epochs() = 0;

    /// Blocks until the given epoch's GPU work has finished, then retires completed epochs.
    /// Does not advance; safe to call from any thread, and used internally for ring back-pressure during recording.
    virtual void wait_for_epoch(epoch e) = 0;

    /// Blocks on the oldest in-flight epoch, then retires — the standard back-pressure primitive when a resource pool is exhausted.
    /// Returns immediately if nothing is in flight.
    /// Does not advance; safe to call from any thread.
    virtual void wait_for_next_inflight_epoch() = 0;

    /// Blocks until a download future is delivered, then returns its bytes.
    /// nullopt if the future is invalid, unsubmitted, or cancelled.
    /// The only completion guarantee for a download — advance_epoch* drain GPU work but not the readback actor.
    /// Waitable once submitted; safe to call from any thread.
    [[nodiscard]] cc::optional<cc::pinned_data<byte const>> wait_for(bytes_future const& future)
    {
        drive_transfers_until_ready(future);
        return future.wait_get_bytes();
    }

    template <class T>
    [[nodiscard]] cc::optional<cc::pinned_data<T const>> wait_for(data_future<T> const& future)
    {
        drive_transfers_until_ready(future);
        return future.wait_get_data();
    }

    /// Blocks until `timestamp`'s tick is delivered, then returns the raw GPU tick.
    /// nullopt if the timestamp is invalid, unsubmitted, or cancelled; waitable once the recording list is submitted.
    /// Only differences are meaningful; normal per-frame usage polls gpu_timestamp::is_ready() instead.
    [[nodiscard]] cc::optional<u64> wait_for_ticks(gpu_timestamp const& timestamp);

    /// Like wait_for_ticks, but returns the tick converted to seconds (1 / timestamp frequency).
    [[nodiscard]] cc::optional<double> wait_for_seconds(gpu_timestamp const& timestamp);

    /// Whether the command list that produced this token has finished executing.
    [[nodiscard]] virtual bool is_submission_complete(submission_token token) const = 0;

    /// Releases all backend resources; the context is unusable afterwards.
    /// Idempotent, and run for you by a backend's destructor.
    virtual void shutdown();

    [[nodiscard]] bool is_shut_down() const { return _is_shut_down; }

protected:
    /// `accepted_shader_formats` must be non-empty, most-preferred first.
    context(backend_kind backend, thread_model threading, cc::span<shader_format const> accepted_shader_formats);

    /// Records which adapter the backend picked.
    /// Called once during creation, before the context is handed out; the adapter cannot change afterwards.
    /// Records which GPU this context runs on, and stamps it into recordings.
    ///
    /// The stamp registration happens here rather than at backend start-up because this is the one point every backend
    /// already goes through, and the first adapter to arrive is the one a recording describes.
    void set_adapter_info(adapter_info info);

    /// Drives cooperative work until `future` is ready or nothing anywhere reports more.
    /// Collapses to a single false test where every semantic thread has an OS thread of its own; without them it is what makes a blocking wait terminate.
    /// Leaving the future unready is fine — the wait below it reports the cancelled / not-yet-submitted cases rather than blocking.
    template <class FutureT>
    void drive_transfers_until_ready(FutureT const& future)
    {
        while (!future.is_ready() && cc::thread_pump_all())
        {
        }
    }

    // Reached by the lifetime scopes (`ctx.persistent.create_raw_buffer(...)`), which funnel here as friends.
    // The try_* virtuals below are the fallible core a backend implements; the public façades add the throwing flavor (see docs/error-handling.md).
    // A backend also implements the public pure virtuals above — submit / drop, the epoch surface, is_submission_complete — and usually overrides shutdown.
    friend class context_persistent_scope;
    friend class context_transient_scope;
    friend class context_upload_scope;
    friend class context_download_scope;
    friend class context_stream_scope;
    friend class context_uncached_scope;
    friend class context_cached_scope;

    /// The fallible core behind the public create_command_list(): backends open a recording list here.
    /// Failure must be device loss, in which case mark_device_lost is called, or an internal bug.
    /// The public wrapper turns the former into a throw and the latter into a fatal.
    [[nodiscard]] virtual cc::result<std::unique_ptr<command_list>> try_create_command_list() = 0;

    /// Marks the device permanently lost with a backend-provided reason; idempotent, the first reason sticks.
    /// Backends call this the moment they observe removal — a create failure, a bad submit signal, or a failed fence wait.
    /// They then raise sg::device_lost_exception at the public boundary.
    void mark_device_lost(cc::string reason);

    /// Streams `data` into `buffer` at `offset_in_bytes` on a dedicated copy queue, reached via ctx.upload.
    /// The implementation must hold the pin until the copy has consumed the bytes, and make a later command list that reads the buffer wait on the copy.
    /// Empty data must be a no-op — ctx.upload forwards it here unchecked.
    virtual void async_upload_bytes_to_buffer(raw_buffer_handle buffer,
                                              cc::pinned_data<byte const> data,
                                              isize offset_in_bytes) = 0;

    /// Streams tightly-packed `data` into one region of `texture` off the frame path, reached via ctx.upload.
    /// See libs/graphics/shaped-graphics/docs/concepts/upload.async.md.
    virtual void async_upload_bytes_to_texture(raw_texture_handle texture,
                                               cc::pinned_data<byte const> data,
                                               subresource_index const& subresource,
                                               texture_region const& region) = 0;

    /// Streams `size_in_bytes` from `buffer` at `offset_in_bytes` back to the host on a dedicated copy queue, reached via ctx.download.
    /// The implementation must make the read wait on the last command list that wrote the buffer, and a later command list that writes it wait on the read.
    /// Dropping the returned future must cancel the copy.
    /// A zero-size read must yield a ready, empty future — ctx.download forwards it here unchecked.
    [[nodiscard]] virtual bytes_future async_download_bytes_from_buffer(raw_buffer_handle buffer,
                                                                        isize offset_in_bytes,
                                                                        isize size_in_bytes) = 0;

    /// Streams one region of `texture` back to the host off the frame path, reached via ctx.download, as tightly-packed bytes.
    /// See libs/graphics/shaped-graphics/docs/concepts/download.async.md.
    [[nodiscard]] virtual bytes_future async_download_bytes_from_texture(raw_texture_handle texture,
                                                                         subresource_index const& subresource,
                                                                         texture_region const& region) = 0;

    // Streaming transfers, reached via ctx.stream — the weaker tier.
    //
    // The implementation must NOT give these the automatic command-list synchronization the async pair carries: the
    // whole point is that a streamed extent costs a later reader nothing.
    // Keeping the destination's storage alive across the copy is still required, and must not go through the same
    // stamp, or the reader wait comes back with it.
    //
    // Every teardown path must settle the handle's completion node, cancellation included.
    // A manual async node nobody ever pushes parks its dependents for the process's lifetime.
    //
    // The sg layer has already resolved regions, bounds-checked, rejected empty work and validated `scope` against
    // the resource's usage flags, so a backend receives only real transfers.

    /// Streams `data` into `buffer` at `offset_in_bytes` on the copy queue, at the streaming tier.
    [[nodiscard]] virtual stream_upload_handle stream_bytes_to_buffer(raw_buffer_handle buffer,
                                                                      cc::pinned_data<byte const> data,
                                                                      isize offset_in_bytes,
                                                                      stream_scope scope) = 0;

    /// Streams tightly-packed `data` into one region of `texture` at the streaming tier.
    [[nodiscard]] virtual stream_upload_handle stream_bytes_to_texture(raw_texture_handle texture,
                                                                       cc::pinned_data<byte const> data,
                                                                       subresource_index const& subresource,
                                                                       texture_region const& region,
                                                                       stream_scope scope) = 0;

    /// Streams into `buffer` from a chunk source rather than one resident payload.
    /// The implementation polls the source on its copy actor thread and must never block on it: `not_yet` means
    /// pass this transfer over and fill the window with other work.
    [[nodiscard]] virtual stream_upload_handle stream_source_to_buffer(raw_buffer_handle buffer,
                                                                       std::unique_ptr<stream_source> source,
                                                                       isize offset_in_bytes,
                                                                       stream_scope scope) = 0;

    /// Streams into one region of `texture` from a chunk source; chunk offsets are row-aligned into the region.
    [[nodiscard]] virtual stream_upload_handle stream_source_to_texture(raw_texture_handle texture,
                                                                        std::unique_ptr<stream_source> source,
                                                                        subresource_index const& subresource,
                                                                        texture_region const& region,
                                                                        stream_scope scope) = 0;

    /// Streams `size_in_bytes` from `buffer` back to the host at the streaming tier.
    [[nodiscard]] virtual stream_download_handle stream_bytes_from_buffer(raw_buffer_handle buffer,
                                                                          isize offset_in_bytes,
                                                                          isize size_in_bytes,
                                                                          stream_scope scope) = 0;

    /// Streams one region of `texture` back to the host at the streaming tier.
    [[nodiscard]] virtual stream_download_handle stream_bytes_from_texture(raw_texture_handle texture,
                                                                           subresource_index const& subresource,
                                                                           texture_region const& region,
                                                                           stream_scope scope) = 0;

    /// Streams `size_in_bytes` from `buffer` into `sink` rather than into a resident destination.
    /// The implementation calls the sink on its copy actor thread, in the transfer's own chunk order, with spans
    /// pointing into the readback staging window — so it must neither block nor retain them.
    [[nodiscard]] virtual stream_download_handle stream_to_sink_from_buffer(raw_buffer_handle buffer,
                                                                            stream_sink sink,
                                                                            isize offset_in_bytes,
                                                                            isize size_in_bytes,
                                                                            stream_scope scope) = 0;

    /// Streams one region of `texture` into `sink`, a run of whole tightly-packed rows at a time.
    [[nodiscard]] virtual stream_download_handle stream_to_sink_from_texture(raw_texture_handle texture,
                                                                             stream_sink sink,
                                                                             subresource_index const& subresource,
                                                                             texture_region const& region,
                                                                             stream_scope scope) = 0;

    // Streaming scheduling knobs, reached via ctx.stream.
    // Defaults are no-ops so a backend without a streaming tier simply ignores them.

    /// Share of copied bytes streaming is owed, per direction (see ctx.stream.set_upload_ratio).
    virtual void set_stream_upload_ratio(float ratio) { (void)ratio; }
    virtual void set_stream_download_ratio(float ratio) { (void)ratio; }

    /// Priority gained per second waiting, per direction (see ctx.stream.set_upload_aging).
    virtual void set_stream_upload_aging(float per_second) { (void)per_second; }
    virtual void set_stream_download_aging(float per_second) { (void)per_second; }

    // Runtime transfer-resource resizing, reached via ctx.upload / ctx.download.
    // Each records a pending change applied at a later safe point — an epoch boundary, or the copy actor between windows — never synchronously here.
    // `bytes` must be > 0; the default no-op means a backend without the path ignores it.

    /// Resize the async-upload staging window (see ctx.upload.set_async_window_size).
    virtual void set_async_upload_window_bytes(isize bytes) { (void)bytes; }

    /// Resize the async-download staging window (see ctx.download.set_async_window_size).
    virtual void set_async_download_window_bytes(isize bytes) { (void)bytes; }

    /// Resize the inline-upload ring (see ctx.upload.set_inline_budget).
    virtual void set_inline_upload_budget(isize bytes) { (void)bytes; }

    /// Resize the inline-download (readback) ring (see ctx.download.set_budget).
    virtual void set_inline_download_budget(isize bytes) { (void)bytes; }

    /// Applies a pending `ctx.transient.set_budget()` at the current epoch boundary, draining in-flight epochs first, then resizing the transient heap.
    /// A backend calls this from advance_epoch once the new epoch is open.
    /// No-op if no budget change is pending.
    void apply_pending_transient_budget() { transient.apply_pending_budget_at_epoch_boundary(); }

    /// Drops the transient bump heap, which is device memory and must not outlive the device.
    /// A backend calls this from its shutdown, before it destroys the device.
    /// The base shutdown also calls it, which covers a backend that forgets — but by then the device may already be
    /// gone, so calling it at the right point is the backend's job.
    void release_transient_heap() { transient.release_heap_at_shutdown(); }

    /// Drops every cached binding-group layout, pipeline layout and pipeline.
    /// Same rule as the two above: they are device objects, and the cache outlives every caller that held one.
    void release_cached_pipelines();

    /// Allocates a GPU-resident buffer; size must be >= 0, and 0 is a valid empty buffer.
    /// `alloc` selects the backing memory (see allocation_info).
    [[nodiscard]] virtual cc::result<raw_buffer_handle> try_create_raw_buffer(isize size_in_bytes,
                                                                              buffer_usages usage,
                                                                              allocation_info const& alloc) = 0;

    /// Allocates a GPU-resident texture from a description; `alloc` selects the backing memory (see allocation_info).
    /// The raw, general create — the typed factories returning `texture<Traits>` (persistent.hh / transient.hh) layer on top of it.
    [[nodiscard]] virtual cc::result<raw_texture_handle> try_create_raw_texture(texture_description const& desc,
                                                                                allocation_info const& alloc) = 0;

    /// Allocates a GPU memory heap of `size_in_bytes` that placed resources sub-allocate into; size must be >= 0, and 0 is a valid empty heap.
    /// A heap is persistent — it outlives the resources placed in it — so it is reached through ctx.persistent.create_memory_heap.
    /// ctx.transient builds on it too: a transient buffer is create_raw_buffer with a heap-placed allocation_info picked by its per-epoch bump allocator.
    [[nodiscard]] virtual cc::result<memory_heap_handle> try_create_memory_heap(isize size_in_bytes) = 0;

    // The bind-path creates carry an explicit lifetime_scope; buffers carry it inside allocation_info instead.
    // binding_group is a real per-scope descriptor allocation, so ctx.persistent / ctx.transient append the scope.
    // Layouts and pipelines are schemas / PSOs with no transient variant — ctx.uncached always passes persistent.

    /// Builds a binding_group_layout (one group's schema) from a shader's reflected bindings.
    /// Any sampler binding named in `static_samplers` is baked into the group layout; the others are dynamic.
    [[nodiscard]] virtual cc::result<binding_group_layout_handle> try_create_binding_group_layout(
        cc::span<binding const> bindings,
        cc::span<named_sampler const> static_samplers,
        lifetime_scope scope) = 0;

    /// Builds a pipeline_layout (the binding interface) from an ordered list of group layouts.
    [[nodiscard]] virtual cc::result<pipeline_layout_handle> try_create_pipeline_layout(
        pipeline_layout_description const& desc,
        lifetime_scope scope) = 0;

    /// Builds a compute_pipeline from a description (compute shader + pipeline layout).
    [[nodiscard]] virtual cc::result<compute_pipeline_handle> try_create_compute_pipeline(
        compute_pipeline_description const& desc,
        lifetime_scope scope) = 0;

    /// Builds a raster_pipeline from a description (vertex/fragment shaders + pipeline layout + state).
    [[nodiscard]] virtual cc::result<raster_pipeline_handle> try_create_raster_pipeline(
        raster_pipeline_description const& desc,
        lifetime_scope scope) = 0;

    /// Builds a raytracing_pipeline (a DXR state object) from a description (shaders + pipeline layout).
    [[nodiscard]] virtual cc::result<raytracing_pipeline_handle> try_create_raytracing_pipeline(
        raytracing_pipeline_description const& desc,
        lifetime_scope scope) = 0;

    /// Builds a raytracing_shader_table over a raytracing_pipeline (references its shader identifiers).
    [[nodiscard]] virtual cc::result<raytracing_shader_table_handle> try_create_raytracing_shader_table(
        raytracing_shader_table_description const& desc,
        lifetime_scope scope) = 0;

    /// Instantiates group `layout` with the given name→view bindings, validated against the layout.
    /// `samplers` supplies one dynamic sampler per non-static sampler binding.
    [[nodiscard]] virtual cc::result<binding_group_handle> try_create_binding_group(binding_group_layout_handle layout,
                                                                                    cc::span<named_view const> views,
                                                                                    cc::span<named_sampler const> samplers,
                                                                                    lifetime_scope scope) = 0;

    /// Builds a staging_binding_group over `layout` — a mutable descriptor image, fully vacant, that mints binding_groups on demand.
    /// `scope` must be persistent: a staging group exists to outlive the epoch that built it.
    [[nodiscard]] virtual cc::result<staging_binding_group_handle> try_create_staging_binding_group(
        binding_group_layout_handle layout,
        lifetime_scope scope) = 0;

    backend_kind _backend;
    thread_model _thread_model;
    bool _is_shut_down = false;

    // Fixed at construction; inline for the realistic one-or-two-format backends, but not capped.
    cc::small_vector<shader_format, 2> _accepted_shader_formats;

    // Filled by the backend during creation, from whatever the API tells it about the adapter it picked.
    adapter_info _adapter;

    // Sticky device-loss state (see is_device_lost), set once via mark_device_lost and never cleared.
    bool _device_lost = false;
    cc::string _device_loss_reason;

    // Built-in pipeline/layout cache reached via ctx.cached.
    // Heap-held so the out-of-line dtor sees the complete type, and this central header need not pull in the cache's std::unordered_map.
    std::unique_ptr<pipeline_cache> _pipeline_cache;

    [[nodiscard]] pipeline_cache& pipeline_cache_ref();
};
