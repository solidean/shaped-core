#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/buffer.hh>

namespace sg::backend::dx12
{
/// DirectX 12 implementation of sg::buffer. Holds the ID3D12Resource (GPU-resident, default heap);
/// null for an empty (size 0) buffer.
class dx12_buffer final : public sg::buffer
{
public:
    dx12_buffer(dx12_context& ctx, cc::isize size_in_bytes, sg::buffer_usage usage, ComPtr<ID3D12Resource> resource)
      : sg::buffer(size_in_bytes, usage), _ctx(ctx), _resource(cc::move(resource))
    {
    }

    dx12_context& _ctx; // creating context — outlives this buffer
    ComPtr<ID3D12Resource> _resource;
};
} // namespace sg::backend::dx12
