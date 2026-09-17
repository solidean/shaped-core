#include "metal_residency.hh"

#include <clean-core/common/assert.hh>

namespace sg::backend::metal
{
cc::result<cc::unit> metal_residency_set::create(MTL::Device* device, MTL4::CommandQueue* queue)
{
    CC_ASSERT(device != nullptr && queue != nullptr, "a residency set needs a device and a queue");

    auto* const descriptor = MTL::ResidencySetDescriptor::alloc()->init();
    descriptor->setLabel(ns_string("sg context residency"));

    NS::Error* error = nullptr;
    auto* const set = device->newResidencySet(descriptor, &error);
    descriptor->release();
    if (set == nullptr)
        return metal_error(error, "the metal device refused a residency set");

    // Attached once.
    // Everything added to the set from here on is resident for work committed to this queue.
    queue->addResidencySet(set);

    _set.lock([&](MTL::ResidencySet*& s) { s = set; });
    return cc::unit{};
}

void metal_residency_set::attach_to(MTL4::CommandQueue* queue)
{
    CC_ASSERT(queue != nullptr, "cannot attach a residency set to a null queue");
    _set.lock(
        [&](MTL::ResidencySet*& s)
        {
            if (s != nullptr)
                queue->addResidencySet(s);
        });
}

void metal_residency_set::add(MTL::Allocation const* allocation)
{
    if (allocation == nullptr)
        return;

    _set.lock(
        [&](MTL::ResidencySet*& s)
        {
            if (s == nullptr)
                return;
            s->addAllocation(allocation);
            // The commit is what publishes the change; an uncommitted add is not resident.
            s->commit();
        });
}

void metal_residency_set::remove(MTL::Allocation const* allocation)
{
    if (allocation == nullptr)
        return;

    _set.lock(
        [&](MTL::ResidencySet*& s)
        {
            if (s == nullptr)
                return;
            s->removeAllocation(allocation);
            s->commit();
        });
}

void metal_residency_set::shutdown()
{
    _set.lock(
        [](MTL::ResidencySet*& s)
        {
            if (s == nullptr)
                return;
            s->removeAllAllocations();
            s->commit();
            s->release();
            s = nullptr;
        });
}
} // namespace sg::backend::metal
