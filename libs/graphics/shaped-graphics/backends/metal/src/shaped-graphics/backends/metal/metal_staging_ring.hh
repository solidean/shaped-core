#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/container/span.hh>
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
/// **A head and a tail, not a bump-and-reset.**
/// Reservations advance the head; an epoch boundary records where it stood, and retiring that epoch moves the tail there.
/// Resetting the head outright at retire is the obvious shape and is wrong: reservations made after the advance belong to
/// a newer epoch and already sit past that point, so rewinding hands the same bytes out twice and the older transfer's
/// data is overwritten before its copy runs.
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
    [[nodiscard]] reservation reserve(isize size);

    /// Where the head stands now, to be handed back to `release_to` when the epoch that ends here retires.
    [[nodiscard]] isize mark() const { return _head; }

    /// Reclaim everything staged before `mark`. Called when the epoch that ended there has retired.
    void release_to(isize mark);

    [[nodiscard]] isize capacity() const { return _capacity; }

    /// The backing buffer, so the context can declare it resident.
    [[nodiscard]] MTL::Buffer* buffer() const { return _buffer; }

    /// Releases the backing buffer; the ring is unusable afterwards.
    void shutdown();

private:
    MTL::Device* _device = nullptr;
    MTL::Buffer* _buffer = nullptr;
    isize _capacity = 0;

    /// Next free byte, and the oldest byte still in use by an epoch that has not retired.
    /// Both only grow; they rewind together once everything staged has been reclaimed.
    isize _head = 0;
    isize _tail = 0;
};
