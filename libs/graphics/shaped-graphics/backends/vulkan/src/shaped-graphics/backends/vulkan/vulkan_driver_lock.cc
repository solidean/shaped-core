#include <shaped-graphics/backends/vulkan/vulkan_driver_lock.hh>

std::shared_mutex& sg::backend::vulkan::driver_lifecycle_lock()
{
    // Function-local so its construction is ordered, and never destroyed: a context torn down during static
    // destruction would otherwise reach a dead lock, which is worse than leaking one mutex.
    static auto* const lock = new std::shared_mutex();
    return *lock;
}
