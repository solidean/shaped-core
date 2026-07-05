#pragma once

#include <clean-core/container/span.hh>
#include <clean-core/error/result.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

namespace sg
{
/// Resource factory for a context's *transient* lifetime scope, reached as `ctx.transient`. Transient
/// resources are tied to the current epoch and recycled when it retires (see lifetime_scope) — per-frame
/// scratch (intermediate buffers, one-shot binding groups) that never needs to outlive the work that
/// produced it. Using one past its epoch is a hard error.
///
/// Buffers are sub-allocated by a simple **per-epoch bump allocator** over one persistent memory_heap
/// the scope owns: the head resets to 0 whenever the epoch changes, so successive epochs alias the same
/// storage. That is safe — a direct queue executes each epoch's GPU work before the next's, so the
/// memory is free by the time it is reused — and cheaper than a ring (smaller heap, better cache
/// behaviour). Requests larger than the budget fall back to a dedicated (committed) allocation. This is
/// all built on the public create_memory_heap + create_buffer; the backend only sees a transient,
/// heap-placed allocation_info.
class context_transient_scope
{
public:
    /// Allocates a transient buffer of `size_in_bytes` (>= 0, 0 is a valid empty buffer) from the
    /// current epoch's bump window; the storage is reused once the epoch changes.
    [[nodiscard]] cc::result<buffer_handle> create_buffer(isize size_in_bytes, buffer_usage usage);

    /// Instantiates `layout` with the given name->view bindings as a transient binding_group (validated
    /// against the layout), whose descriptors are recycled when this epoch retires.
    [[nodiscard]] cc::result<binding_group_handle> create_binding_group(binding_layout_handle layout,
                                                                        cc::span<named_view const> views);

    /// Sets the transient-buffer heap budget in bytes (the per-epoch bump capacity). Must be called
    /// before the first transient buffer; the heap is created lazily on first use. Default: 128 MiB.
    void set_buffer_budget(isize size_in_bytes);

    // Pinned to its owning context: neither copyable nor movable.
    context_transient_scope(context_transient_scope const&) = delete;
    context_transient_scope(context_transient_scope&&) = delete;
    context_transient_scope& operator=(context_transient_scope const&) = delete;
    context_transient_scope& operator=(context_transient_scope&&) = delete;

private:
    friend class context;
    explicit context_transient_scope(context& ctx) : _ctx(ctx) {}

    context& _ctx;

    // The bump allocator state: the heap (lazy), its budget, the current head, and the epoch the head
    // was last reset for. Guarded — create_buffer may run on any thread.
    struct bump_state
    {
        memory_heap_handle heap = nullptr;
        isize budget = isize(128) * 1024 * 1024;
        isize head = 0;
        u64 last_epoch = 0; // sg::epoch value the head was last reset for (0 = never)
    };
    cc::mutex<bump_state> _bump;
};
} // namespace sg
