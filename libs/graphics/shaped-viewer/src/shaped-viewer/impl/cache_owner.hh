#pragma once

#include <clean-core/thread/atomic.hh>
#include <shaped-viewer/fwd.hh>

namespace sv::impl
{
/// Names the manager a value's placement cache was filled by, never recycled within the process.
///
/// **An address cannot serve here.**
/// A cached value outlives its manager, and the next manager is routinely allocated where the last one was, so a
/// pointer compare would trust ids minted by a dead manager — ids the new one resolves to different resources.
/// 0 is "no owner", what a value nobody has placed holds.
enum class cache_owner : u64
{
    none = 0
};

/// A fresh owner, distinct from every other this process has handed out.
[[nodiscard]] inline cache_owner next_cache_owner()
{
    static cc::atomic<u64> s_next = {1};
    return cache_owner(s_next.fetch_add(1, cc::memory_order_relaxed));
}
} // namespace sv::impl
