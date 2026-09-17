#include "metal_staging_ring.hh"

#include <clean-core/string/format.hh>

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

[[nodiscard]] u64 align_up(u64 value, u64 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}
} // namespace

metal_staging_ring::~metal_staging_ring()
{
    shutdown();
}

cc::result<cc::unit> metal_staging_ring::create(MTL::Device* device, isize capacity_in_bytes, cc::string_view label)
{
    CC_ASSERT(_buffer == nullptr, "a staging ring is created once");
    CC_ASSERT(capacity_in_bytes > 0, "a staging ring needs a positive capacity");

    _device = device;
    _buffer = device->newBuffer(NS::UInteger(capacity_in_bytes), k_staging_options);
    if (_buffer == nullptr)
        return cc::error(cc::format("the metal device refused a {} byte staging ring", capacity_in_bytes));

    _buffer->setLabel(ns_string(label));
    _capacity = capacity_in_bytes;
    _state.lock(
        [](ring_state& s)
        {
            s = {};
            s.open_copies = cc::make_shared<std::atomic<int>>(0);
        });
    return cc::unit{};
}

metal_staging_ring::reservation metal_staging_ring::reserve(isize size)
{
    CC_ASSERT(_buffer != nullptr, "the staging ring has no storage");
    CC_ASSERT(size >= 0, "a reservation must be non-negative");

    auto const capacity = u64(_capacity);
    auto const from_ring = _state.lock(
        [&](ring_state& s) -> cc::optional<isize>
        {
            if (u64(size) > capacity)
                return {}; // larger than the whole ring: no reclaim makes this fit

            // A checkpoint held back by a copy that has since drained is freed here rather than at the next retire,
            // which may never come — an epoch is told about its completion once.
            reclaim(s);

            auto start = align_up(s.next_pos, u64(k_reservation_alignment));

            // A reservation is one contiguous span, so one that would straddle the seam skips to it instead.
            // The skipped tail is still charged against the cursor below, which is what keeps the fullness test honest.
            if (start % capacity + u64(size) > capacity)
                start = (start / capacity + 1) * capacity;

            auto const end = start + u64(size);
            if (end - s.freed_pos > capacity)
                return {}; // the space is still held by epochs that have not retired

            s.next_pos = end;
            return isize(start % capacity);
        });

    if (from_ring.has_value())
        return {.buffer = _buffer, .offset = from_ring.value(), .size = size};

    // No room, so this transfer gets storage of its own rather than an error.
    // A single transfer larger than the whole ring lands here too, and is perfectly legitimate.
    auto* const dedicated = _device->newBuffer(NS::UInteger(size > 0 ? size : 1), k_staging_options);
    if (dedicated == nullptr)
        return {}; // an invalid reservation: the caller's transfer fails rather than the process
    dedicated->setLabel(ns_string("sg staging overflow"));

    return {.buffer = dedicated, .offset = 0, .size = size, .owned = dedicated};
}

cc::shared_ptr<std::atomic<int>> metal_staging_ring::account_pending_copy()
{
    return _state.lock(
        [](ring_state& s)
        {
            s.open_copies->fetch_add(1, std::memory_order_acq_rel);
            return s.open_copies;
        });
}

void metal_staging_ring::on_epoch_advance(sg::epoch closed)
{
    _state.lock(
        [&](ring_state& s)
        {
            s.checkpoints.push_back({closed, s.next_pos, cc::move(s.open_copies)});
            s.open_copies = cc::make_shared<std::atomic<int>>(0);
        });
}

void metal_staging_ring::on_epochs_completed(sg::epoch completed)
{
    _state.lock(
        [&](ring_state& s)
        {
            s.completed_epoch = cc::max(s.completed_epoch, u64(completed));
            reclaim(s);
        });
}

void metal_staging_ring::reclaim(ring_state& s)
{
    auto retired = isize(0);
    for (auto const& cp : s.checkpoints)
    {
        if (u64(cp.epoch_id) > s.completed_epoch)
            break;
        if (cp.outstanding != nullptr && cp.outstanding->load(std::memory_order_acquire) != 0)
            break; // a download still owes a copy out of these bytes, whatever the fence says

        s.freed_pos = cp.end_pos; // checkpoints are monotonic in epoch and in end_pos
        ++retired;
    }
    s.checkpoints.remove_from_to(0, retired);
}

metal_staging_ring::debug_cursors metal_staging_ring::debug_cursor_state()
{
    return _state.lock(
        [](ring_state& s)
        { return debug_cursors{.next_pos = s.next_pos, .freed_pos = s.freed_pos, .checkpoints = s.checkpoints.size()}; });
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
    _state.lock(
        [](ring_state& s)
        {
            s = {};
            s.open_copies = cc::make_shared<std::atomic<int>>(0);
        });
}
} // namespace sg::backend::metal
