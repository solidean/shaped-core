#pragma once

#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/command_list.hh>

namespace sg::backend::dx12
{
/// DirectX 12 implementation of sg::command_list. Owns its allocator and graphics command list and
/// is handed out already recording (record once, submit once — not reused). The concrete recording
/// methods (copy/upload/download buffer, ...) land here with the first milestone.
///
/// A command list is consumed by submit/drop or simply destroyed on scope exit; single-submit and
/// single-drop are structural (the owning unique_ptr is moved in), so no lifecycle flags are needed.
/// The _ctx backref is where future resource-tracking teardown will unwind on destruction.
class dx12_command_list final : public sg::command_list
{
public:
    dx12_command_list(dx12_context& ctx, ComPtr<ID3D12CommandAllocator> allocator, ComPtr<ID3D12GraphicsCommandList> list)
      : _ctx(ctx), _allocator(cc::move(allocator)), _list(cc::move(list))
    {
    }

    dx12_context& _ctx; // creating context; must outlive this command list (global lifetime invariant)
    ComPtr<ID3D12CommandAllocator> _allocator;
    ComPtr<ID3D12GraphicsCommandList> _list;
};
} // namespace sg::backend::dx12
