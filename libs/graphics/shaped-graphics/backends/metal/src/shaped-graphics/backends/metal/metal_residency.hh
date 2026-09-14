#pragma once

#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/backends/metal/fwd.hh>
#include <shaped-graphics/backends/metal/metal_common.hh>
#include <shaped-graphics/fwd.hh>

/// Everything this context's GPU work may touch, declared resident.
///
/// **Metal 4 removed `useResource`.** A command buffer no longer names the resources it reads; an `MTLResidencySet`
/// attached to the queue does, and a resource outside every set the queue knows about is simply not there when the GPU
/// runs — a copy from it reads zeroes and a copy to it writes nowhere.
/// Nothing reports this: it is not an API misuse, so the validation layer says nothing either.
///
/// **One set for the whole context**, rather than one per command list.
/// That is coarser than Metal allows and it is the honest starting point: a per-list set would have to be built from
/// the list's touched-resource tracking and committed per submit, which is a cost worth paying only once there is a
/// working frame to measure it against.
/// libs/graphics/shaped-graphics/backends/metal/readme.md records it as the optimization it is.
///
/// Every mutation is followed by `commit`, which is what publishes the change to the queue.
class sg::backend::metal::metal_residency_set
{
public:
    /// Creates the set and attaches it to `queue`. Must be called once, before any resource is added.
    void create(MTL::Device* device, MTL4::CommandQueue* queue);

    /// Declare `allocation` resident.
    /// Null is ignored, which is what an empty buffer hands over.
    void add(MTL::Allocation const* allocation);

    /// Stop declaring `allocation` resident.
    /// Null is ignored.
    void remove(MTL::Allocation const* allocation);

    /// Releases the set; it is unusable afterwards.
    void shutdown();

private:
    cc::mutex<MTL::ResidencySet*> _set;
};
