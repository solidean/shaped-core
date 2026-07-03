#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Lifetime mode of a placement — a hard contract, not a hint. `persistent` memory lives until its
/// handles are released. `transient` memory expires when its epoch retires; using a transient resource
/// beyond that epoch is a hard error, and the backend may recycle the memory immediately. (Both modes
/// still get in-flight GPU hazard tracking; that is orthogonal to the lifetime.)
enum class allocation_scope
{
    persistent,
    transient,
};

/// A value describing where a resource's backing GPU memory lives — a cheap, copyable placement handle,
/// not an owner of the GPU resource itself. Produced by a memory_heap (or hand-built for the dedicated
/// case) and passed to a create_* call.
///
/// `heap` null vs non-null is the load-bearing convention:
///   - null  => self-allocating: the resource gets its own dedicated allocation (`offset`/`size` unused).
///   - set   => placed: the resource is sub-allocated into `heap` at `offset`, sharing the heap's
///              underlying allocation. Holding the memory_heap_handle keeps that heap alive.
///
/// Trivially copyable in spirit (it only carries a handle + plain fields); copying it does not copy or
/// own any GPU resource.
struct allocation_info
{
    /// Owning heap this placement points into, or null for a dedicated (self-allocating) resource.
    memory_heap_handle heap = nullptr;

    /// Byte offset of the placement within `heap`. Ignored when `heap` is null.
    isize offset = 0;

    /// Byte size of the placement. Ignored when `heap` is null (the resource sizes its own allocation).
    isize size_in_bytes = 0;

    /// Lifetime mode of the allocation (see allocation_scope).
    allocation_scope scope = allocation_scope::persistent;

    /// True when the resource owns its allocation (no heap) instead of being placed into a shared one —
    /// a "committed resource" in dx12 terms. Equivalent to `heap == nullptr`.
    [[nodiscard]] bool is_dedicated() const { return heap == nullptr; }

    /// True when the resource is sub-allocated into a shared heap. Equivalent to `heap != nullptr`.
    [[nodiscard]] bool is_placed() const { return heap != nullptr; }
};
} // namespace sg
