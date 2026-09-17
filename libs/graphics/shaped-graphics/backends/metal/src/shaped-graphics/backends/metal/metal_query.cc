#include "metal_query.hh"

#include <clean-core/record/log.hh>
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

    // The probe is a real heap rather than a capability query, because there is no capability query: a device either
    // hands one out or reports why it cannot.
    // It goes straight onto the free list, so the probe is also the first lease.
    auto probe = create_heap();
    _supports_timestamps = probe != nullptr;
    if (probe != nullptr)
        _free_list.lock([&](cc::vector<cc::unique_ptr<metal_counter_heap_lease>>& free)
                        { free.push_back(cc::move(probe)); });

    return cc::unit{};
}

cc::unique_ptr<metal_counter_heap_lease> metal_query_system::create_heap()
{
    auto* const descriptor = MTL4::CounterHeapDescriptor::alloc()->init();
    descriptor->setType(MTL4::CounterHeapTypeTimestamp);
    descriptor->setCount(NS::UInteger(metal_query_system::SlotsPerHeap));

    NS::Error* error = nullptr;
    auto* const heap = _ctx->device()->newCounterHeap(descriptor, &error);
    descriptor->release();

    if (heap == nullptr)
    {
        CC_LOG_WARNING("the metal device refused a timestamp counter heap: {}",
                       error != nullptr ? error->localizedDescription()->utf8String() : "no reason given");
        return nullptr;
    }

    auto lease = cc::make_unique<metal_counter_heap_lease>();
    lease->heap = heap;
    lease->slot_count = metal_query_system::SlotsPerHeap;
    return lease;
}

cc::unique_ptr<metal_counter_heap_lease> metal_query_system::acquire_heap()
{
    if (!_supports_timestamps)
        return nullptr;

    auto pooled = _free_list.lock(
        [](cc::vector<cc::unique_ptr<metal_counter_heap_lease>>& free) -> cc::unique_ptr<metal_counter_heap_lease>
        {
            if (free.empty())
                return nullptr;
            auto lease = cc::move(free.back());
            free.remove_back();
            return lease;
        });

    if (pooled != nullptr)
        return pooled;
    return create_heap();
}

void metal_query_system::release_heap(cc::unique_ptr<metal_counter_heap_lease> lease)
{
    if (lease == nullptr)
        return;

    // Invalidated rather than left as they are: a resolve of a slot never written reads whatever the last leaseholder
    // put there, and a heap is reused across lists that have nothing to do with each other.
    if (lease->next_slot > 0)
        lease->heap->invalidateCounterRange(NS::Range(0, NS::UInteger(lease->next_slot)));

    lease->next_slot = 0;
    lease->shared_future = std::make_shared<sg::data_future<u64>>();

    _free_list.lock([&](cc::vector<cc::unique_ptr<metal_counter_heap_lease>>& free) { free.push_back(cc::move(lease)); });
}

void metal_query_system::shutdown()
{
    _free_list.lock(
        [](cc::vector<cc::unique_ptr<metal_counter_heap_lease>>& free)
        {
            for (auto& lease : free)
                lease->heap->release();
            free.clear();
        });
    _supports_timestamps = false;
}
} // namespace sg::backend::metal
