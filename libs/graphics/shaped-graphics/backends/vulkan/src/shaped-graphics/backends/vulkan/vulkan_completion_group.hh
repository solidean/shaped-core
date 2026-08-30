#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>

/// One completion timeline for one resource in one direction.
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
/// dx12 reaches this with an ID3D12Fence and its own counter; a Vulkan timeline semaphore already *is* that pair,
/// so the only state beside the handle is the reservation counter — which the semaphore deliberately does not own,
/// since a value must be reserved on the caller's thread before the copy that will signal it is even queued.
///
/// The counter is never reset, not even when a group is recycled onto another resource.
/// A fresh resource's first value therefore continues wherever its predecessor stopped rather than starting at 1,
/// which is exactly what keeps recycling safe: every value it hands out is strictly higher than anything the
/// semaphore has already signaled, so no wait on it can be satisfied early.
struct sg::backend::vulkan::vulkan_completion_group
{
    vulkan_context* ctx = nullptr;
    VkSemaphore timeline = VK_NULL_HANDLE;

    /// Reserved on caller threads at enqueue, in the order the jobs are handed to the actor.
    /// That order is what the family rule then preserves all the way to completion.
    cc::atomic<u64> next_value = {0};

    /// Highest value signaled so far.
    /// Touched only by the actor that owns this direction, so it needs no synchronization.
    u64 last_signaled = 0;

    [[nodiscard]] u64 reserve() { return next_value.fetch_add(1, cc::memory_order_relaxed) + 1; }

    /// Whether the copy carrying `value` has run; `0` means nothing was ever reserved, so trivially yes.
    /// Body in vulkan_completion_group.cc, which has vulkan_context complete.
    [[nodiscard]] bool has_reached(u64 value) const;
};

/// A completion value together with the timeline it belongs to.
///
/// Both halves are needed to wait: the value alone is meaningless now that every resource counts independently.
/// A null `group` (or a zero `value`) means "nothing pending", which is what an untouched resource reports and what
/// every wait site treats as already satisfied.
struct sg::backend::vulkan::vulkan_group_value
{
    vulkan_completion_group_handle group;
    u64 value = 0;

    [[nodiscard]] bool is_pending() const { return group != nullptr && value != 0; }
    [[nodiscard]] bool has_reached() const { return !is_pending() || group->has_reached(value); }

    /// Raises this to `other` when the two name the same timeline, and reports whether it could.
    /// False means they are unrelated values that a caller has to keep side by side — merging them into one maximum
    /// is exactly the mistake per-resource timelines exist to prevent.
    [[nodiscard]] bool try_raise_to(vulkan_group_value const& other)
    {
        if (!other.is_pending())
            return true; // nothing to raise to
        if (group == nullptr)
        {
            *this = other;
            return true;
        }
        if (group != other.group)
            return false;
        if (other.value > value)
            value = other.value;
        return true;
    }
};

/// Hands out completion groups and takes them back, so a program that creates and destroys many resources does not
/// grow one timeline semaphore per resource ever created.
///
/// Returning is safe precisely because a group is reference-counted: it comes back only once the resource, every
/// in-flight job, every recorded command list and every deferred deletion holding it are all gone — by which point
/// each of its values has long since been signaled.
/// The counter continuing rather than resetting is what makes reuse safe (see the note on the group itself).
class sg::backend::vulkan::vulkan_completion_group_pool
{
public:
    /// Must be called before the first acquire.
    void initialize(vulkan_context& ctx);

    /// A group ready to hand out, its timeline already created.
    /// Thread-safe: resources are created from any thread.
    [[nodiscard]] vulkan_completion_group_handle acquire();

    /// Destroys the free list's semaphores, before the device goes.
    /// A group still referenced by a live resource keeps its own semaphore alive and simply has nowhere to return to
    /// afterwards — which is the teardown-order trap a reference-counted device would have hidden.
    void shutdown();

private:
    /// The free list, held behind a shared_ptr so a group's deleter can find it — or find it gone.
    /// A group outliving its pool is normal at teardown, and must destroy its semaphore rather than resurrect
    /// anything.
    struct free_list
    {
        cc::mutex<cc::vector<vulkan_completion_group*>> groups;
        cc::atomic<bool> alive = {true};
    };

    vulkan_context* _ctx = nullptr;
    std::shared_ptr<free_list> _free;
};
