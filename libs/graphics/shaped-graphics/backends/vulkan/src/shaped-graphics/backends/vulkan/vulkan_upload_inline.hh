#pragma once

#include <clean-core/thread/atomic.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/vulkan/fwd.hh>
#include <shaped-graphics/backends/vulkan/vulkan_common.hh>
#include <shaped-graphics/fwd.hh>

/// The staging ring behind `cmd.upload` — host-visible memory a command list copies out of on the GPU timeline.
///
/// This is the first host-visible allocation in the backend.
/// Everything before it asked find_memory_type for DEVICE_LOCAL only, which is the root reason every transfer path
/// was a stub: there was nowhere for the CPU to write bytes the GPU could then copy.
///
/// The ring is a logical u64 cursor mapped onto physical storage by `% capacity`, the same model dx12 uses.
/// A copy never straddles the seam: a reservation that would wrap is placed at the start of the ring instead, so the
/// tail is skipped rather than split.
/// Space is reclaimed at **epoch** granularity — each epoch records where it ended, and its span frees when that
/// epoch retires — because the bytes are read by a GPU copy that only the epoch fence can prove has finished.

struct sg::backend::vulkan::vulkan_upload_allocation
{
    VkBuffer buffer = VK_NULL_HANDLE; ///< the ring's buffer, which the copy reads from
    isize offset = 0;                 ///< byte offset of this reservation within it
    byte* mapped = nullptr;           ///< where to write the bytes; never null for a non-empty reservation
};

class sg::backend::vulkan::vulkan_upload_inline_system
{
public:
    /// Allocates the ring, once, from context creation.
    /// `capacity_in_bytes` must be > 0.
    [[nodiscard]] cc::result<cc::unit> initialize(vulkan_context& ctx, isize capacity_in_bytes);

    /// Destroys the ring.
    /// Safe to call twice, and on an uninitialized system.
    void shutdown();

    /// Reserves `size_in_bytes` of contiguous ring space for the current epoch, at an offset that is a multiple of
    /// `alignment_in_bytes`.
    ///
    /// A buffer copy needs no alignment, but an image copy does: Vulkan requires bufferOffset to be a multiple of 4
    /// and of the texel block size, so a texture upload passes that constraint in rather than hoping for it.
    ///
    /// Blocks on an in-flight epoch when the ring is full, which is the back-pressure that bounds it.
    ///
    /// Where waiting cannot help it falls back to a one-off staging buffer instead of failing:
    /// a single upload larger than the whole ring, which no budget fixes, and one epoch's uploads exceeding the ring
    /// with nothing in flight to reclaim.
    /// The fallback is correct but slow — a dedicated allocation per transfer — so it warns once per epoch with the
    /// size and the budget, which is what a caller needs to set the budget properly.
    [[nodiscard]] vulkan_upload_allocation reserve(isize size_in_bytes, isize alignment_in_bytes = 1);

    /// Records a pending ring capacity (> 0), applied at the next epoch boundary (apply_pending_budget).
    /// Deferred rather than immediate because the GPU may still be reading the ring this call would free.
    void set_budget(isize capacity);

    /// Applies a pending set_budget at an epoch boundary, and is a no-op when nothing is pending.
    /// Drains every in-flight epoch first, so nothing is still reading the ring being replaced.
    void apply_pending_budget();

    /// Records where the closing epoch ended, so its span can be freed when it retires.
    void on_epoch_advance(sg::epoch closed);

    /// Frees the span of every epoch up to and including `completed`.
    void on_epochs_completed(sg::epoch completed);

    [[nodiscard]] isize capacity() const { return _capacity; }

private:
    struct checkpoint
    {
        sg::epoch epoch_id;
        u64 end_pos;
    };

    struct ring_state
    {
        u64 next_pos = 0;                   ///< logical write cursor; only ever increases
        u64 freed_pos = 0;                  ///< everything below this has been reclaimed
        cc::vector<checkpoint> checkpoints; ///< FIFO, monotonic in both epoch and end_pos
        isize pending_capacity = 0;         ///< a set_budget awaiting the next epoch boundary (0 = none)
    };

    /// One host-visible mapped buffer, the shape both initialize and a resize build.
    struct ring_storage
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        byte* mapped = nullptr;
    };

    /// Builds a mapped TRANSFER_SRC buffer of `capacity` bytes, or nullopt if it could not be allocated.
    [[nodiscard]] cc::optional<ring_storage> create_ring(isize capacity);

    /// A dedicated staging buffer for one transfer the ring could not hold.
    /// Freed with the epoch that recorded the copy, which is the same lifetime the ring span would have had.
    [[nodiscard]] vulkan_upload_allocation reserve_outside_ring(isize size_in_bytes);

    /// Says so once per epoch, naming what did not fit and what the budget is.
    void warn_outside_ring(isize size_in_bytes);

    vulkan_context* _ctx = nullptr;
    VkBuffer _buffer = VK_NULL_HANDLE;
    VkDeviceMemory _memory = VK_NULL_HANDLE;
    byte* _mapped = nullptr; ///< persistently mapped for the ring's whole life
    isize _capacity = 0;
    cc::mutex<ring_state> _state;

    /// The last epoch the fallback warning fired in, so a frame that overruns repeatedly says so once.
    cc::atomic<u64> _last_warned_epoch = 0;
};
