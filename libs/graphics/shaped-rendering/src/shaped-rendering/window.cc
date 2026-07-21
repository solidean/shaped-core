#include <clean-core/common/utility.hh> // cc::move
#include <shaped-rendering/window.hh>

// The backend-independent half of the window API: the throwing façades over the try_ functions.
// Everything that actually touches a platform lives in impl/, in one backend or the other.

namespace sr
{
cc::unique_ptr<window_system> window_system::create(window_system_description const& desc)
{
    return try_create(desc).or_throw();
}

cc::unique_ptr<window> window_system::create_window(window_description const& desc)
{
    return try_create_window(desc).or_throw();
}
} // namespace sr
