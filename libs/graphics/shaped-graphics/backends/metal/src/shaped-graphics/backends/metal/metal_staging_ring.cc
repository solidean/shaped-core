#include "metal_staging_ring.hh"

namespace sg::backend::metal
{
namespace
{
/// Every reservation starts here, which is what keeps a copy's source and destination offsets legal on every device.
/// 256 is the strictest of the alignments Metal asks for across buffer and texture copies, so one constant covers both
/// rather than the ring having to know what it is staging for.
constexpr isize k_reservation_alignment = 256;

/// The resource options both rings and their overflow buffers are made with.
/// Shared rather than private: this is the one place sg does want CPU-visible storage, and it is the whole reason the
/// ring exists.
constexpr MTL::ResourceOptions k_staging_options
    = MTL::ResourceStorageModeShared | MTL::ResourceHazardTrackingModeUntracked;
} // namespace

metal_staging_ring::~metal_staging_ring()
{
    shutdown();
}

void metal_staging_ring::create(MTL::Device* device, isize capacity_in_bytes, cc::string_view label)
{
    CC_ASSERT(_buffer == nullptr, "a staging ring is created once");
    CC_ASSERT(capacity_in_bytes > 0, "a staging ring needs a positive capacity");

    _device = device;
    _buffer = device->newBuffer(NS::UInteger(capacity_in_bytes), k_staging_options);
    CC_ASSERT(_buffer != nullptr, "the metal device refused a staging ring allocation");

    _buffer->setLabel(ns_string(label));
    _capacity = capacity_in_bytes;
    _head = 0;
    _tail = 0;
}

metal_staging_ring::reservation metal_staging_ring::reserve(isize size)
{
    CC_ASSERT(_buffer != nullptr, "the staging ring has no storage");
    CC_ASSERT(size >= 0, "a reservation must be non-negative");

    // Everything staged has been reclaimed, so the head may start over.
    // This is the only moment it is safe: while anything is in flight, rewinding would hand out bytes a pending copy
    // still reads.
    if (_head == _tail)
    {
        _head = 0;
        _tail = 0;
    }

    auto const aligned_head = (_head + k_reservation_alignment - 1) / k_reservation_alignment * k_reservation_alignment;
    if (aligned_head + size <= _capacity)
    {
        _head = aligned_head + size;
        return {.buffer = _buffer, .offset = aligned_head, .size = size};
    }

    // No room this epoch, so this transfer gets storage of its own rather than an error.
    // A single transfer larger than the whole ring lands here too, and is perfectly legitimate.
    auto* const dedicated = _device->newBuffer(NS::UInteger(size > 0 ? size : 1), k_staging_options);
    CC_ASSERT(dedicated != nullptr, "the metal device refused a dedicated staging allocation");
    dedicated->setLabel(ns_string("sg staging overflow"));

    return {.buffer = dedicated, .offset = 0, .size = size, .owned = dedicated};
}

void metal_staging_ring::release_to(isize mark)
{
    CC_ASSERT(mark >= 0 && mark <= _capacity, "a staging mark must be inside the ring");
    // Only ever forward: epochs retire in order, and a stale mark from a rewind must not pull the tail back.
    if (mark > _tail)
        _tail = mark;
}

void metal_staging_ring::shutdown()
{
    if (_buffer == nullptr)
        return;

    // Released directly rather than deferred: the context drains the GPU before it shuts its rings down, so nothing in
    // flight still names these bytes.
    _buffer->release();
    _buffer = nullptr;
    _device = nullptr;
    _capacity = 0;
    _head = 0;
    _tail = 0;
}
} // namespace sg::backend::metal
