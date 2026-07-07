#pragma once

#include <clean-core/container/pinned_data.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/bytes_future.hh>
#include <shaped-graphics/context.download.hh>
#include <shaped-graphics/context.persistent.hh>
#include <shaped-graphics/context.transient.hh>
#include <shaped-graphics/context.upload.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

#include <memory>

namespace sg
{
/// Mutable entry point to a graphics backend: the factory for command lists and GPU resources.
/// Abstract — a backend subclasses it (e.g. sg::backend::vulkan::vulkan_context) and you obtain one
/// from that backend's sg::create_<backend>_context(config).
///
/// Must outlive every command list and resource it creates, and be shut down before destruction.
class context
{
public:
    virtual ~context();

    /// The backend kind driving this context — a coarse tag, not the concrete type.
    [[nodiscard]] backend_kind backend() const { return _backend; }

    /// The threading guarantees this backend provides (see the threading concept doc).
    [[nodiscard]] thread_model threading() const { return _thread_model; }

    /// Persistent-lifetime resource factory. Create long-lived GPU resources through it:
    /// `ctx.persistent.create_buffer(...)`.
    context_persistent_scope persistent;

    /// Transient-lifetime resource factory for per-frame scratch, recycled when the epoch retires:
    /// `ctx.transient.create_buffer(...)`. See lifetime_scope.
    context_transient_scope transient;

    /// Async host→device upload facade: `ctx.upload.bytes_to_buffer(...)`. Streams buffer writes on a
    /// dedicated copy queue, off the frame path (the context-level mirror of the inline cmd.upload).
    context_upload_scope upload;

    /// Inline download configuration facade: `ctx.download.set_budget(...)`. Downloads are recorded via
    /// cmd.download; this scope sizes the shared readback ring they stage through.
    context_download_scope download;

    /// Opens a new command list, already recording. Single-use: submit or drop it once.
    [[nodiscard]] virtual cc::result<std::unique_ptr<command_list>> create_command_list() = 0;

    /// Submits a command list for execution and consumes it, returning a token for its completion.
    /// A command list must be submitted (or dropped) in the same epoch it was opened in.
    virtual submission_token submit_command_list(std::unique_ptr<command_list> cmd) = 0;

    /// Discards a command list unsubmitted and consumes it — same as letting it go out of scope.
    /// Must happen in the epoch the list was opened in.
    virtual void drop_command_list(std::unique_ptr<command_list> cmd) = 0;

    // Epochs — frame-level GPU lifetime + CPU↔GPU sync.
    // See libs/graphics/shaped-graphics/docs/concepts/epochs.md.
public:
    /// The epoch new work is currently recorded into.
    [[nodiscard]] virtual epoch current_epoch() const = 0;

    /// The latest epoch whose GPU work has fully finished (its resources are reclaimable).
    [[nodiscard]] virtual epoch completed_epoch() const = 0;

    /// Closes the current epoch and opens the next: all its GPU work is gated behind one fence
    /// value, this epoch's garbage becomes reclaimable once that fence signals. Every command list
    /// opened this epoch must already be submitted or dropped. Advancing is a deliberate, rationed
    /// operation — it also bounds how far the CPU runs ahead of the GPU.
    ///
    /// `allowed_in_flight` throttles pipelining depth: nullopt never waits; 0 fully drains the GPU
    /// before returning; N keeps at most N prior epochs in flight (a windowed renderer typically
    /// passes its swapchain back-buffer count).
    virtual void advance_epoch(cc::optional<int> allowed_in_flight) = 0;

    /// Advance the epoch and block until the GPU is fully idle (equivalent to advance_epoch(0)).
    /// Idle drains GPU work but not the readback actor — an inline-download future may still be
    /// undelivered right after; use wait_for(future) to be certain.
    virtual void advance_epoch_and_wait_for_idle() = 0;

    /// Reclaims everything owned by epochs the GPU has finished. Safe to call at any time and from any
    /// thread (but not concurrently with advance_epoch); also runs implicitly after the waits below.
    virtual void process_completed_epochs() = 0;

    /// Blocks until the given epoch's GPU work has finished, then retires completed epochs. Does not
    /// advance. Safe to call from any thread (used internally for ring back-pressure during recording).
    virtual void wait_for_epoch(epoch e) = 0;

    /// Blocks on the oldest in-flight epoch, then retires — the standard back-pressure primitive when a
    /// resource pool is exhausted. Returns immediately if nothing is in flight. Does not advance; safe
    /// to call from any thread.
    virtual void wait_for_next_inflight_epoch() = 0;

    /// Blocks until a download future is delivered, then returns its bytes (nullopt if invalid,
    /// unsubmitted, or cancelled). The only completion guarantee for a download — advance_epoch* drain
    /// GPU work but not the readback actor. Waitable once submitted; safe to call from any thread.
    [[nodiscard]] cc::optional<cc::pinned_data<cc::byte const>> wait_for(bytes_future const& future)
    {
        return future.wait_get_bytes();
    }

    template <class T>
    [[nodiscard]] cc::optional<cc::pinned_data<T const>> wait_for(data_future<T> const& future)
    {
        return future.wait_get_data();
    }

    /// Whether the command list that produced this token has finished executing.
    [[nodiscard]] virtual bool is_submission_complete(submission_token token) const = 0;

    /// Releases all backend resources; the context is unusable afterwards. Idempotent, and run
    /// automatically on destruction.
    virtual void shutdown();

    /// Whether shutdown() has run.
    [[nodiscard]] bool is_shut_down() const { return _is_shut_down; }

protected:
    context(backend_kind backend, thread_model threading);

    // Reached by the lifetime scopes (`ctx.persistent.create_buffer(...)`), which funnel here as friends.
    friend class context_persistent_scope;
    friend class context_transient_scope;
    friend class context_upload_scope;
    friend class context_download_scope;

    /// Streams `data` into `buffer` at `offset_in_bytes` on a dedicated copy queue (reached via
    /// ctx.upload). The pin holds the source bytes alive until the copy consumes them; a later command
    /// list that reads the buffer automatically waits on the copy. Empty data is a no-op. Buffer must
    /// have buffer_usage::copy_dst; offset_in_bytes + data.size() must be within the buffer.
    virtual void async_upload_bytes_to_buffer(buffer_handle buffer,
                                              cc::pinned_data<cc::byte const> data,
                                              isize offset_in_bytes)
        = 0;

    // Runtime transfer-resource resizing (reached via ctx.upload / ctx.download). Each records a pending
    // change applied at a later safe point (an epoch boundary, or the copy actor between windows), never
    // synchronously here. `bytes` must be > 0. Default: no-op — a backend without the path ignores it.

    /// Resize the async-upload staging window (see ctx.upload.set_async_window_size).
    virtual void set_async_upload_window_bytes(isize bytes) { (void)bytes; }

    /// Resize the inline-upload ring (see ctx.upload.set_inline_budget).
    virtual void set_inline_upload_budget(isize bytes) { (void)bytes; }

    /// Resize the inline-download (readback) ring (see ctx.download.set_budget).
    virtual void set_inline_download_budget(isize bytes) { (void)bytes; }

    /// Applies a pending `ctx.transient.set_budget()` at the current epoch boundary (draining in-flight
    /// epochs first, then resizing the transient heap). A backend calls this from advance_epoch once the
    /// new epoch is open. No-op if no budget change is pending.
    void apply_pending_transient_budget() { transient.apply_pending_budget_at_epoch_boundary(); }

    /// Allocates a GPU-resident buffer. Size must be >= 0 (0 is a valid empty buffer). `alloc` selects
    /// the backing memory (see allocation_info).
    [[nodiscard]] virtual cc::result<buffer_handle> create_buffer(isize size_in_bytes,
                                                                  buffer_usage usage,
                                                                  allocation_info const& alloc)
        = 0;

    /// Allocates a GPU memory heap of `size_in_bytes` that placed resources sub-allocate into. Size
    /// must be >= 0 (0 is a valid empty heap that holds no placements). A heap is persistent — it
    /// outlives the resources placed in it — so it is reached through ctx.persistent.create_memory_heap.
    /// ctx.transient also builds on it: a transient buffer is just create_buffer with a transient,
    /// heap-placed allocation_info picked by ctx.transient's per-epoch bump allocator.
    [[nodiscard]] virtual cc::result<memory_heap_handle> create_memory_heap(isize size_in_bytes) = 0;

    // The bind-path creates carry an explicit lifetime_scope (persistent vs transient); the
    // ctx.persistent / ctx.transient facades append it. (Buffers carry it inside allocation_info instead.)

    /// Builds a binding_layout (the bindable-set schema) from a shader's reflected bindings.
    [[nodiscard]] virtual cc::result<binding_layout_handle> create_binding_layout(cc::span<binding const> bindings,
                                                                                  lifetime_scope scope)
        = 0;

    /// Builds a compute_pipeline from a description (compute shader + layout).
    [[nodiscard]] virtual cc::result<compute_pipeline_handle> create_compute_pipeline(compute_pipeline_description const& desc,
                                                                                      lifetime_scope scope)
        = 0;

    /// Instantiates `layout` with the given name→view bindings, validating each against the layout.
    [[nodiscard]] virtual cc::result<binding_group_handle> create_binding_group(binding_layout_handle layout,
                                                                                cc::span<named_view const> views,
                                                                                lifetime_scope scope)
        = 0;

    backend_kind _backend;
    thread_model _thread_model;
    bool _is_shut_down = false;
};
} // namespace sg
