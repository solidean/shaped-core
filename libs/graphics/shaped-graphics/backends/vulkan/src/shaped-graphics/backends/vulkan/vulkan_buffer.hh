#pragma once

#include <shaped-graphics/buffer.hh>

namespace sg::backend::vulkan
{
/// Vulkan implementation of sg::buffer. Derives directly and inherits the protected shape members
/// (_size_in_bytes / _usage); the vulkan resource handles land here as the backend is implemented.
class vulkan_buffer final : public sg::buffer
{
public:
    vulkan_buffer(cc::isize size_in_bytes, sg::buffer_usage usage) : sg::buffer(size_in_bytes, usage) {}
};
} // namespace sg::backend::vulkan
