#include "metal_query.hh"

#include <clean-core/string/format.hh>
#include <shaped-graphics/backends/metal/metal_context.hh>

namespace sg::backend::metal
{
namespace
{
/// Metal reports GPU timestamps on the CPU's timebase, in nanoseconds.
/// Measured rather than assumed — see metal_query_system::timestamp_tick_to_seconds.
constexpr double k_tick_to_seconds = 1e-9;
} // namespace

cc::result<cc::unit> metal_query_system::create(metal_context& ctx)
{
    _ctx = &ctx;
    _tick_to_seconds = k_tick_to_seconds;

    // Not probed with a real heap, for the reason `sg::feature::ray_query` is not probed either: a timestamp counter
    // heap is core to Metal 4, so every device above this backend's floor has one.
    //
    // **Probing would cost a heap per context**, and heaps are a device-wide resource with a limit the tier-2 suite
    // reaches on its own — every context paying for a capability most never use is what exhausts it.
    // So the first heap is made when the first timestamp is recorded, and a device that refuses one then reports a
    // device error and hands the caller an invalid timestamp, which is the answer an unsupported backend gives anyway.
    _supports_timestamps = true;
    return cc::unit{};
}

cc::unique_ptr<metal_counter_heap_lease> metal_query_system::acquire_lease()
{
    if (!_supports_timestamps)
        return nullptr;

    // Made under the lock, so lists racing for the last range make one heap between them rather than one each.
    return _pool.lock(
        [&](pool& p) -> cc::unique_ptr<metal_counter_heap_lease>
        {
            if (p.free.empty())
            {
                auto* const descriptor = MTL4::CounterHeapDescriptor::alloc()->init();
                descriptor->setType(MTL4::CounterHeapTypeTimestamp);
                descriptor->setCount(NS::UInteger(SlotsPerHeap));

                NS::Error* error = nullptr;
                auto* const heap = _ctx->device()->newCounterHeap(descriptor, &error);
                descriptor->release();

                if (heap == nullptr)
                {
                    _ctx->report_feedback_error(
                        sg::device_error_kind::creation_failed,
                        cc::format("the metal device refused a timestamp counter heap beside the {} this context "
                                   "holds: {}",
                                   p.heaps.size(),
                                   error != nullptr ? error->localizedDescription()->utf8String() : "no reason given"));
                    return nullptr;
                }

                p.heaps.push_back(heap);
                // Pushed last to first, so ranges come out in slot order.
                for (auto first = SlotsPerHeap - SlotsPerLease; first >= 0; first -= SlotsPerLease)
                {
                    auto lease = cc::make_unique<metal_counter_heap_lease>();
                    lease->heap = heap;
                    lease->first_slot = first;
                    lease->slot_count = SlotsPerLease;
                    p.free.push_back(cc::move(lease));
                }
            }

            auto lease = cc::move(p.free.back());
            p.free.remove_back();
            return lease;
        });
}

void metal_query_system::release_lease(cc::unique_ptr<metal_counter_heap_lease> lease)
{
    if (lease == nullptr)
        return;

    // Invalidated rather than left as they are: a resolve of a slot never written reads whatever the last leaseholder
    // put there, and a range is reused across lists that have nothing to do with each other.
    if (lease->next_slot > 0)
        lease->heap->invalidateCounterRange(NS::Range(NS::UInteger(lease->first_slot), NS::UInteger(lease->next_slot)));

    lease->next_slot = 0;
    lease->shared_future = std::make_shared<sg::data_future<u64>>();

    _pool.lock([&](pool& p) { p.free.push_back(cc::move(lease)); });
}

void metal_query_system::shutdown()
{
    _pool.lock(
        [](pool& p)
        {
            p.free.clear();
            for (auto* const heap : p.heaps)
                heap->release();
            p.heaps.clear();
        });
    _supports_timestamps = false;
}
} // namespace sg::backend::metal
