#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Lifetime hint the backend honors for a placement. `transient` marks memory expected to live for a
/// frame/epoch rather than indefinitely, letting the backend recycle it aggressively — but it is only a
/// hint: the backend still tracks in-flight GPU usage, so a transient allocation never waives hazard
/// tracking. `persistent` is the default long-lived scope.
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

    /// Lifetime hint the backend honors (see allocation_scope).
    allocation_scope scope = allocation_scope::persistent;
};
} // namespace sg
