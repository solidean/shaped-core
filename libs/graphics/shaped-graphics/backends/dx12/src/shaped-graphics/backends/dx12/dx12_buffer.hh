#pragma once

#include <shaped-graphics/buffer.hh>

namespace sg::backend::dx12
{
/// DirectX 12 implementation of sg::buffer. Derives directly and inherits the protected shape
/// members (_size_in_bytes / _usage); the dx12 resource handles land here as the backend is
/// implemented.
class dx12_buffer final : public sg::buffer
{
public:
    dx12_buffer(cc::isize size_in_bytes, sg::buffer_usage usage) : sg::buffer(size_in_bytes, usage) {}
};
} // namespace sg::backend::dx12
