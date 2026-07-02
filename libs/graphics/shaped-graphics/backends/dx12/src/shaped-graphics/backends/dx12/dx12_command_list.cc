// dx12_command_list: allocation, submission, and drop. The list type is header-only (ctor +
// fields); its create/submit/drop bodies live here.

#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
cc::result<std::unique_ptr<dx12_command_list>> dx12_context::create_dx12_command_list()
{
    ComPtr<ID3D12CommandAllocator> allocator;
    if (HRESULT hr = _device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
        FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommandAllocator failed");

    ComPtr<ID3D12GraphicsCommandList> list;
    if (HRESULT hr
        = _device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommandList failed");

    // Left open (recording); submit closes it.
    return std::make_unique<dx12_command_list>(*this, cc::move(allocator), cc::move(list));
}

void dx12_context::submit_dx12_command_list(std::unique_ptr<dx12_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot submit a null command list");

    // Close, then execute. NOTE: no fence yet — destroying the list right after is only safe because
    // today's lists carry no real work. Fencing + device-loss handling land with the sync milestone.
    HRESULT hr = cmd->_list->Close();
    CC_ASSERT(SUCCEEDED(hr), "ID3D12GraphicsCommandList::Close failed");

    ID3D12CommandList* lists[] = {cmd->_list.Get()};
    _queue->ExecuteCommandLists(1, lists);
}

void dx12_context::drop_dx12_command_list(std::unique_ptr<dx12_command_list> cmd)
{
    CC_ASSERT(cmd != nullptr, "cannot drop a null command list");
    // cmd is destroyed here — the explicit form of letting it leave scope. Teardown lives in the dtor.
}
} // namespace sg::backend::dx12
