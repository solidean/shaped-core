#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>

/// A shared-storage MTLBuffer that inline transfers stage through, reclaimed an epoch at a time.
///
/// **This is the piece Metal makes trivial and Vulkan makes hard.**
/// All twelve of the vulkan backend's transfer stubs traced back to one missing thing: a host-visible memory type to
/// write bytes into.
/// On unified memory `MTLStorageModeShared` is the default answer rather than a hunt — the CPU pointer is just
/// `contents()`.
///
/// **Two logical cursors that only ever increase**, with the byte offset derived as `cursor % capacity` at the point of
/// use, and fullness tested as the difference `end - freed > capacity`.
/// That is the shape dx12 and vulkan already use, and the reason is that the obvious alternative cannot be made safe:
/// a head and a tail rewound to zero once they meet hands the same bytes out twice.
/// An epoch that stages nothing captures the same boundary as the epoch before it, so after a rewind the second retire
/// arrives carrying a pre-rewind boundary, drags the tail past the head, and the next reservation sees an empty ring
/// while reservations are still live.
/// Nothing asserts on that; it surfaces as corrupted transfer bytes somewhere else entirely.
///
/// **The ring owns the invariant**, rather than trusting a caller to hand back a boundary that is still meaningful.
/// Epoch boundaries are checkpoints in its own state, retired by walking them against the completed epoch.
///
/// A reservation the ring cannot fit gets a dedicated buffer of its own instead of failing.
/// A transfer larger than the whole ring is legitimate, and so is a frame that stages more than the budget.
/// Neither should be an error a caller cannot act on.
class sg::backend::metal::metal_staging_ring
{
public:
    /// A reserved span of the ring: where the CPU writes (or reads), and where the GPU copy addresses.
    struct reservation
    {
        MTL::Buffer* buffer = nullptr;
        isize offset = 0;
        isize size = 0;

        /// Non-null when this reservation got a buffer of its own because the ring could not fit it.
        /// The caller owns it and must hand it to the epoch; `buffer` points at the same object.
        MTL::Buffer* owned = nullptr;

        /// The CPU-visible bytes of this reservation.
        [[nodiscard]] cc::span<byte> bytes() const
        {
            return cc::span<byte>(static_cast<byte*>(buffer->contents()) + offset, size);
        }

        [[nodiscard]] bool is_valid() const { return buffer != nullptr; }
    };

    metal_staging_ring() = default;
    ~metal_staging_ring();

    metal_staging_ring(metal_staging_ring const&) = delete;
    metal_staging_ring& operator=(metal_staging_ring const&) = delete;

    /// Allocate the backing buffer.
    /// Must be called once, before any reservation.
    void create(MTL::Device* device, isize capacity_in_bytes, cc::string_view label);

    /// Reserve `size` bytes, from the ring where it fits and from a dedicated buffer where it does not.
    /// Never fails: a reservation always comes back valid.
    ///
    /// The span is always contiguous, so a request that would straddle the seam skips to it and leaves the tail unused.
    /// dx12 splits at the seam instead and has its callers walk the windows; here one reservation is one span, which is
    /// what every call site expects.
    [[nodiscard]] reservation reserve(isize size);

    /// Records where the closing epoch's staging ends, so its bytes are reclaimed when it retires.
    void on_epoch_advance(sg::epoch closed);

    /// Frees every checkpoint up to and including `completed`.
    /// Walking rather than assigning is what makes a repeated or out-of-order retire harmless.
    void on_epochs_completed(sg::epoch completed);

    [[nodiscard]] isize capacity() const { return _capacity; }

    /// The backing buffer, so the context can declare it resident.
    [[nodiscard]] MTL::Buffer* buffer() const { return _buffer; }

    /// Releases the backing buffer; the ring is unusable afterwards.
    void shutdown();

    // --- test-only escape hatch ----------------------------------------------------------------------
    // The tier-2 tests assert cursor behaviour directly, because the defect this shape exists to prevent is invisible
    // from the outside until bytes are already wrong — see libs/graphics/shaped-graphics/docs/testing.md.

    struct debug_cursors
    {
        u64 next_pos = 0;
        u64 freed_pos = 0;
        isize checkpoints = 0;
    };
    [[nodiscard]] debug_cursors debug_cursor_state();

private:
    MTL::Device* _device = nullptr;
    MTL::Buffer* _buffer = nullptr;
    isize _capacity = 0;

    /// A closed epoch and where its staging ended; its bytes free once that epoch retires.
    struct epoch_checkpoint
    {
        sg::epoch epoch_id = sg::epoch::invalid;
        u64 end_pos = 0;
    };

    struct ring_state
    {
        u64 next_pos = 0;                         ///< logical bump cursor over the u64 space, never rewound
        u64 freed_pos = 0;                        ///< everything logically below this is reclaimable
        cc::vector<epoch_checkpoint> checkpoints; ///< FIFO, oldest epoch at the front
    };

    /// The lock lives here rather than around the whole ring, because the cursors and the checkpoint FIFO have to move
    /// together for the invariant to hold.
    cc::mutex<ring_state> _state;
};
