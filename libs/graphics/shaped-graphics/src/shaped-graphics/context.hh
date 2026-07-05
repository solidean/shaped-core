#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/error/result.hh>
#include <shaped-graphics/context.persistent.hh>
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
    /// `ctx.persistent.create_buffer(...)`. (A transient scope for per-frame resources may follow.)
    context_persistent_scope persistent;

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
    virtual void advance_epoch_and_wait_for_idle() = 0;

    /// Reclaims everything owned by epochs the GPU has finished. Safe to call at any time; also
    /// runs implicitly after the waits below.
    virtual void process_completed_epochs() = 0;

    /// Blocks until the given epoch's GPU work has finished, then retires completed epochs. Does
    /// not advance.
    virtual void wait_for_epoch(epoch e) = 0;

    /// Blocks on the oldest in-flight epoch, then retires — the standard back-pressure primitive
    /// when a resource pool is exhausted. Returns immediately if nothing is in flight. Does not advance.
    virtual void wait_for_next_inflight_epoch() = 0;

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

    /// Allocates a GPU-resident buffer. Size must be >= 0 (0 is a valid empty buffer). `alloc` selects
    /// the backing memory (see allocation_info).
    [[nodiscard]] virtual cc::result<buffer_handle> create_buffer(isize size_in_bytes,
                                                                  buffer_usage usage,
                                                                  allocation_info const& alloc)
        = 0;

    /// Allocates a GPU memory heap of `size_in_bytes` that placed resources sub-allocate into. Size
    /// must be >= 0 (0 is a valid empty heap that holds no placements). A heap is persistent — it
    /// outlives the resources placed in it — so it is reached through ctx.persistent.create_memory_heap.
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
