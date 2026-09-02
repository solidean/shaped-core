#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/fwd.hh>

#include <atomic>

/// One completion timeline for one resource in one direction: a fence, plus the counter handing out its values.
///
/// **Why a timeline per resource rather than one per system.**
/// A window signals "the highest value I finished", which is exact only where completion order matches reservation
/// order.
/// The transfer scheduler guarantees that within one *family* — jobs sharing a destination resource — and
/// deliberately breaks it across families, since overtaking an unrelated blocked transfer is the whole point.
/// A single shared timeline therefore reports a low-numbered job complete the moment any higher-numbered one
/// finishes, which hands a reader bytes that have not been copied yet.
/// So a group *is* a family: one per resource, per direction.
///
/// The counter is never reset, not even when a group is recycled onto another resource.
/// A fresh resource's first value therefore continues wherever its predecessor stopped rather than starting at 1,
/// which is exactly what keeps recycling safe: every value it hands out is strictly higher than anything the fence
/// has already signaled, so no wait on it can be satisfied early.
struct sg::backend::dx12::dx12_completion_group
{
    ComPtr<ID3D12Fence> fence;

    /// Reserved on caller threads at enqueue, in the order the jobs are handed to the actor.
    /// That order is what the family rule then preserves all the way to completion.
    std::atomic<u64> next_value = 0;

    /// Highest value signaled on `fence` so far.
    /// Touched only by the actor that owns this direction, so it needs no synchronization.
    u64 last_signaled = 0;

    [[nodiscard]] u64 reserve() { return next_value.fetch_add(1, std::memory_order_relaxed) + 1; }

    /// Whether the copy carrying `value` has run; `0` means nothing was ever reserved, so trivially yes.
    [[nodiscard]] bool has_reached(u64 value) const { return value == 0 || fence->GetCompletedValue() >= value; }
};

/// A completion value together with the timeline it belongs to.
///
/// Both halves are needed to wait: the value alone is meaningless now that every resource counts independently.
/// A null `group` (or a zero `value`) means "nothing pending", which is what an untouched resource reports and what
/// every wait site treats as already satisfied.
struct sg::backend::dx12::dx12_group_value
{
    dx12_completion_group_handle group;
    u64 value = 0;

    [[nodiscard]] bool is_pending() const { return group != nullptr && value != 0; }
    [[nodiscard]] bool has_reached() const { return !is_pending() || group->has_reached(value); }

    /// Raises this to `other` when the two name the same timeline, and reports whether it could.
    /// False means they are unrelated values that a caller has to keep side by side — merging them into one maximum
    /// is exactly the mistake per-resource timelines exist to prevent.
    [[nodiscard]] bool try_raise_to(dx12_group_value const& other)
    {
        if (group != other.group)
            return false;
        if (other.value > value)
            value = other.value;
        return true;
    }
};

namespace sg::backend::dx12
{
/// Raise `slot` to `value`, never lower it.
/// Every cross-queue stamp is monotonic and never reset, so a racing higher value simply wins and a stale one yields a
/// cheap already-satisfied wait.
inline void stamp_max(std::atomic<u64>& slot, u64 value)
{
    u64 prev = slot.load(std::memory_order_relaxed);
    while (prev < value && !slot.compare_exchange_weak(prev, value, std::memory_order_release, std::memory_order_relaxed))
    {
        // CAS retries; `prev` is refreshed with the current value each time.
    }
}
} // namespace sg::backend::dx12

/// Hands out completion groups and takes them back when their last owner drops them.
///
/// Recycling is what keeps the fence count proportional to the resources that can be copied rather than growing
/// with every resource ever created.
/// Returning is safe precisely because a group is reference-counted: it comes back only once the resource, every
/// in-flight job, every recorded command list and every deferred deletion holding it are all gone — by which point
/// each of its values has long since been signaled.
class sg::backend::dx12::dx12_completion_group_pool
{
public:
    /// Must be called before the first acquire; `device` is what fences are created on.
    void initialize(ID3D12Device* device);

    /// A group ready to hand out, its fence already created.
    /// Thread-safe: resources are created from any thread.
    [[nodiscard]] dx12_completion_group_handle acquire();

    /// Drops the free list.
    /// Groups still referenced by live resources keep their fences alive on their own, and simply have nowhere to
    /// return to afterwards.
    void shutdown();

private:
    /// The free list, held behind a `shared_ptr` so a group's deleter can find it — or find it gone.
    /// A group outliving its pool is normal at teardown, and must destroy its fence rather than resurrect anything.
    struct free_list
    {
        cc::mutex<cc::vector<dx12_completion_group*>> groups;
    };

    ID3D12Device* _device = nullptr;
    std::shared_ptr<free_list> _free;
};
