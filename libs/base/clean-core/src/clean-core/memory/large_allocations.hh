#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/fwd.hh>
#include <clean-core/thread/atomic.hh>

// =========================================================================================================
// cc::large_allocations — opt-in accounting of large allocations, per context
// =========================================================================================================
//
// Off by default.
// Every allocation and free through clean-core's default memory resources compares its size against a threshold,
// which is the largest isize while off, so the small path pays that one compare and nothing else.
// On (start_tracking), an allocation at or above the threshold is booked to the current context, the u64 the
// file-access recording's hook names (cc::file_access::set_context_hook), and its free is credited back to that same
// context, whichever thread or context frees it, through a table of the live large blocks behind a mutex.
// A large allocation costs microseconds already (fresh pages, an OS mapping), so the bookkeeping is noise beside it.
//
// The default threshold is 1 MiB: past mimalloc's large-object size, where every block comes from its own OS mapping,
// and big enough that an ordinary container growing step by step stays below it until it is worth noticing.
//
// What it does not see, by design:
// * many small allocations summing up (a node-based container of a million entries) — a sampling of the process's
//   memory from outside sees those;
// * memory taken past clean-core's resources: std containers, a third-party malloc, OS mappings, GPU memory;
// * an in-place resize, which never moves a block nor changes its usable size.

namespace cc::large_allocations
{
/// The threshold start_tracking takes when given none.
inline constexpr isize default_threshold = isize(1) << 20;
} // namespace cc::large_allocations

/// What one context allocated in large blocks.
struct cc::large_allocations::context_stats
{
    /// What the context hook returned when the blocks were allocated; 0 for none.
    u64 context = 0;
    /// Bytes of its large blocks alive now, and the most alive at once.
    i64 live_bytes = 0;
    i64 peak_live_bytes = 0;
    /// Large blocks allocated, and their bytes summed.
    i64 allocations = 0;
    i64 allocated_bytes = 0;
};

namespace cc::large_allocations::impl
{
/// The smallest size booked; the largest isize while not tracking, so no size reaches it.
extern cc::atomic<isize> g_threshold;

void note_allocated(void* p, isize bytes);
void note_freed(void* p);

/// Books a block the resource just handed out, if it is large and tracking is on.
inline void on_allocate(void* p, isize bytes)
{
    if (bytes >= g_threshold.load(cc::memory_order_relaxed)) [[unlikely]]
        note_allocated(p, bytes);
}

/// Credits a block back before the resource takes it, if it is large and tracking is on.
inline void on_deallocate(void* p, isize bytes)
{
    if (bytes >= g_threshold.load(cc::memory_order_relaxed)) [[unlikely]]
        note_freed(p);
}
} // namespace cc::large_allocations::impl

namespace cc::large_allocations
{
/// Starts booking allocations of `threshold` bytes or more; what is booked already stays.
/// A block allocated before tracking started is never booked, so its free credits nothing.
void start_tracking(isize threshold = default_threshold);

/// Stops booking; the stats stay readable through snapshot().
void stop_tracking();

[[nodiscard]] bool is_tracking();

/// Every context that allocated a large block while tracking, in no particular order.
[[nodiscard]] cc::vector<context_stats> snapshot();
} // namespace cc::large_allocations
