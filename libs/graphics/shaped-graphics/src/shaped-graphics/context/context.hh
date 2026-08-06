#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/small_vector.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <clean-core/string/string.hh>
#include <clean-core/string/string_view.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/context/cached.hh>
#include <shaped-graphics/context/download.hh>
#include <shaped-graphics/context/persistent.hh>
#include <shaped-graphics/context/transient.hh>
#include <shaped-graphics/context/uncached.hh>
#include <shaped-graphics/context/upload.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/present/swapchain.hh>
#include <shaped-graphics/resource/texture_region.hh>
#include <shaped-graphics/routine/routine_registry.hh>
#include <shaped-graphics/types.hh>

#include <memory>

namespace sg
{
/// Mutable entry point to a graphics backend: the factory for command lists and GPU resources.
/// Abstract — a backend subclasses it (e.g. sg::backend::vulkan::vulkan_context), and you obtain one from that backend's sg::create_<backend>_context(config).
/// Must outlive every command list and resource it creates.
/// A backend's destructor runs shutdown() for you; call it yourself only to release the device early.
class context
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

    /// The threading guarantees this backend provides (see libs/graphics/shaped-graphics/docs/concepts/threading.md).
    [[nodiscard]] thread_model threading() const { return _thread_model; }

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

    /// Runs one cycle of the upload/download copy actors' work, returning true if there may be more.
    /// Where the platform has threads the actors drive themselves, so this returns false without doing anything.
    ///
    /// It is load-bearing only under CC_HAS_THREADS == 0, where a cc::threaded_actor runs on its caller and nothing advances a copy unless someone pumps it.
    /// Every blocking wait must therefore drain this first — including GPU waits, since an async upload's copy-queue fence is signalled by the actor.
    /// Backends with no actors can leave this alone.
    virtual bool pump_transfers() { return false; }

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

    /// Pumps transfers until `future` is ready or no transfer work is left.
    /// Collapses to a single false test where the platform has threads; without them it is what makes a blocking wait terminate.
    /// Leaving the future unready is fine — the wait below it reports the cancelled / not-yet-submitted cases rather than blocking.
    template <class FutureT>
    void drive_transfers_until_ready(FutureT const& future)
    {
        while (!future.is_ready() && pump_transfers())
        {
        }
    }

    // Reached by the lifetime scopes (`ctx.persistent.create_raw_buffer(...)`), which funnel here as friends.
    // The try_* virtuals below are the fallible core a backend implements; the public façades add the throwing flavor (see docs/error-handling.md).
    // A backend also implements the public pure virtuals above — submit / drop, the epoch surface, is_submission_complete — and usually overrides shutdown and pump_transfers.
    friend class context_persistent_scope;
    friend class context_transient_scope;
    friend class context_upload_scope;
    friend class context_download_scope;
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

    /// Allocates a GPU-resident buffer; size must be >= 0, and 0 is a valid empty buffer.
    /// `alloc` selects the backing memory (see allocation_info).
    [[nodiscard]] virtual cc::result<raw_buffer_handle> try_create_raw_buffer(isize size_in_bytes,
                                                                              buffer_usage usage,
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

    backend_kind _backend;
    thread_model _thread_model;
    bool _is_shut_down = false;

    // Fixed at construction; inline for the realistic one-or-two-format backends, but not capped.
    cc::small_vector<shader_format, 2> _accepted_shader_formats;

    // Sticky device-loss state (see is_device_lost), set once via mark_device_lost and never cleared.
    bool _device_lost = false;
    cc::string _device_loss_reason;

    // Built-in pipeline/layout cache reached via ctx.cached.
    // Heap-held so the out-of-line dtor sees the complete type, and this central header need not pull in the cache's std::unordered_map.
    std::unique_ptr<pipeline_cache> _pipeline_cache;

    [[nodiscard]] pipeline_cache& pipeline_cache_ref();
};
} // namespace sg
