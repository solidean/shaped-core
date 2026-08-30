#pragma once

#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>

/// Host-visible memory that descriptors are written into, and that shaders read through a bound device address.
///
/// This is the descriptor-buffer model, and it is why a binding group here is a *range* rather than an opaque object:
/// minting a group is copying bytes into this heap, which is what makes a staging group's snapshot affordable.
///
/// Persistent ranges come from a first-fit free list, so a released group's space is reusable and a long-lived group
/// does not fragment the heap by outliving a bump cursor.
/// Transient ranges bump from a separate region that resets when the epoch changes, matching how transient buffers
/// already work — except that these are written by the CPU and read by the GPU, so a slot cannot be reused until the
/// epoch that wrote it retires.
/// That is the same hazard the upload ring has, and it is why the transient region checkpoints per epoch rather
/// than resetting to zero.

/// One allocated range within the heap; move-only so a range has exactly one owner.
struct sg::backend::vulkan::vulkan_descriptor_range
{
    isize offset = 0; ///< byte offset into the heap
    isize size = 0;   ///< byte length
    bool transient = false;

    [[nodiscard]] bool is_empty() const { return size == 0; }
};

class sg::backend::vulkan::vulkan_descriptor_heap
{
public:
    /// Allocates the heap.
    /// `capacity_in_bytes` must be > 0, and `transient_fraction` is the share reserved for epoch-scoped groups.
    [[nodiscard]] cc::result<cc::unit> initialize(vulkan_context& ctx, isize capacity_in_bytes, float transient_fraction);

    void shutdown();

    /// A persistent range of `size_in_bytes`, aligned to the device's descriptor offset alignment.
    /// An empty range means the heap is exhausted, which is a recoverable runtime failure rather than a contract
    /// violation — a large bindless table is exactly the case that hits it.
    [[nodiscard]] vulkan_descriptor_range allocate_persistent(isize size_in_bytes);

    /// Returns a persistent range to the free list, coalescing with its neighbours.
    void free_persistent(vulkan_descriptor_range range);

    /// A range valid for the current epoch only.
    [[nodiscard]] vulkan_descriptor_range allocate_transient(isize size_in_bytes);

    void on_epoch_advance(sg::epoch closed);
    void on_epochs_completed(sg::epoch completed);

    /// Where to write a range's descriptors.
    [[nodiscard]] byte* mapped_at(vulkan_descriptor_range const& range) const { return _mapped + range.offset; }

    /// The heap's device address, which is what a descriptor-buffer binding names.
    [[nodiscard]] VkDeviceAddress device_address() const { return _device_address; }

    [[nodiscard]] VkBuffer buffer() const { return _buffer; }

private:
    struct free_range
    {
        isize offset = 0;
        isize size = 0;
    };

    struct checkpoint
    {
        sg::epoch epoch_id;
        u64 end_pos;
    };

    struct heap_state
    {
        cc::vector<free_range> free_ranges; ///< persistent region, sorted by offset, coalesced on free
        u64 transient_next = 0;             ///< logical bump cursor over the transient region
        u64 transient_freed = 0;
        cc::vector<checkpoint> checkpoints;
    };

    vulkan_context* _ctx = nullptr;
    VkBuffer _buffer = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
    byte* _mapped = nullptr;
    VkDeviceAddress _device_address = 0;
    isize _capacity = 0;
    isize _transient_begin = 0; ///< the transient region starts here; the persistent one is everything below
    isize _alignment = 4;
    cc::mutex<heap_state> _state;
};
