#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/command_list.hh>

namespace sg::backend::dx12
{
/// DirectX 12 implementation of sg::command_list. Owns its allocator and graphics command list,
/// handed out already recording. Recording methods (copy/upload/download buffer, ...) land here with
/// the first milestone.
class dx12_command_list final : public sg::command_list
{
public:
    dx12_command_list(dx12_context& ctx,
                      sg::epoch created_in,
                      ComPtr<ID3D12CommandAllocator> allocator,
                      ComPtr<ID3D12GraphicsCommandList> list)
      : sg::command_list(created_in), _ctx(ctx), _allocator(cc::move(allocator)), _list(cc::move(list))
    {
    }

    dx12_context& _ctx; // creating context — outlives this list
    ComPtr<ID3D12CommandAllocator> _allocator;
    ComPtr<ID3D12GraphicsCommandList> _list;
};
} // namespace sg::backend::dx12
