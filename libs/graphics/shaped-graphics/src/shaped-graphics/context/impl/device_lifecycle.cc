#include "device_lifecycle.hh"

#include <clean-core/common/assert.hh>

#include <shared_mutex>

namespace
{
std::shared_mutex& lifecycle_lock()
{
    // Never destroyed: a context torn down during static destruction would otherwise reach a dead lock.
    static auto* const lock = new std::shared_mutex();
    return *lock;
}

thread_local int tl_exclusive_depth = 0;
thread_local int tl_shared_depth = 0;
} // namespace

sg::impl::device_lifecycle_hold::device_lifecycle_hold()
{
    if (tl_exclusive_depth++ > 0)
        return;
    CC_ASSERT(tl_shared_depth == 0,
              "device creation or teardown inside a ray-tracing driver call would deadlock on the "
              "device lifecycle lock");
    lifecycle_lock().lock();
    _owns = true;
}

sg::impl::device_lifecycle_hold::~device_lifecycle_hold()
{
    --tl_exclusive_depth;
    if (_owns)
        lifecycle_lock().unlock();
}

sg::impl::raytracing_driver_hold::raytracing_driver_hold()
{
    // Under this thread's own exclusive hold there is nothing to take, and nothing to count.
    if (tl_exclusive_depth > 0)
        return;
    _counted = true;
    if (tl_shared_depth++ > 0)
        return;
    lifecycle_lock().lock_shared();
    _owns = true;
}

sg::impl::raytracing_driver_hold::~raytracing_driver_hold()
{
    if (_counted)
        --tl_shared_depth;
    if (_owns)
        lifecycle_lock().unlock_shared();
}
