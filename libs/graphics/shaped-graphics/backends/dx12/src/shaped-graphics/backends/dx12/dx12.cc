// dx12 backend implementation: the heavier backend-typed bodies — device bring-up, resource
// creation, submit/drop, shutdown. Small forwarders and ctors stay inline in the headers.

#include <shaped-graphics/backends/dx12/dx12_context.hh>

// ID3D12Debug (the debug-layer interface) lives in the SDK-layers header, separate from d3d12.h.
#include <d3d12sdklayers.h>

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

cc::result<dx12_buffer_handle> dx12_context::create_dx12_buffer(cc::isize size_in_bytes, sg::buffer_usage usage)
{
    CC_ASSERT(size_in_bytes >= 0, "buffer size must be non-negative");

    ComPtr<ID3D12Resource> resource;

    // Empty buffer: no allocation (D3D12 rejects a zero-width resource); null is the representation.
    if (size_in_bytes > 0)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT; // GPU-resident: sg exposes no host-visible buffers.

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = UINT64(size_in_bytes);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; // required for buffers
        desc.Flags = sg::has_flag(usage, sg::buffer_usage::storage) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                                    : D3D12_RESOURCE_FLAG_NONE;

        D3D12_RESOURCE_STATES initial_state = sg::has_flag(usage, sg::buffer_usage::copy_dst)
                                                ? D3D12_RESOURCE_STATE_COPY_DEST
                                                : D3D12_RESOURCE_STATE_COMMON;

        if (HRESULT hr = _device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial_state, nullptr,
                                                          IID_PPV_ARGS(&resource));
            FAILED(hr))
            return dx12_error(hr, "ID3D12Device::CreateCommittedResource failed");
    }

    return std::make_shared<dx12_buffer>(*this, size_in_bytes, usage, cc::move(resource));
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

void dx12_context::shutdown()
{
    if (_is_shut_down)
        return;
    // Release the device-level COM objects (live-object tracking will unwind here later too).
    _queue.Reset();
    _device.Reset();
    _factory.Reset();
    _is_shut_down = true;
}
} // namespace sg::backend::dx12

namespace sg
{
cc::result<context_handle> create_dx12_context(backend::dx12::dx12_config const& config)
{
    using namespace sg::backend::dx12;

    UINT factory_flags = 0;
    if (config.enable_debug_layer)
    {
        // Best-effort: needs the "Graphics Tools" feature; skip validation if it's absent, don't fail.
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }

    ComPtr<IDXGIFactory4> factory;
    if (HRESULT hr = CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory)); FAILED(hr))
        return dx12_error(hr, "CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter;
    if (config.use_warp)
    {
        if (HRESULT hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)); FAILED(hr))
            return dx12_error(hr, "IDXGIFactory4::EnumWarpAdapter failed");
    }
    else
    {
        bool found = false;
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 ad = {};
            adapter->GetDesc1(&ad);
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue; // WARP is opt-in via use_warp, not a silent fallback.

            // Null out-param probes D3D12 support (FL 11_0) without creating a device.
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            {
                found = true;
                break;
            }
        }
        if (!found)
            return cc::error("no Direct3D 12 capable hardware adapter found");
    }

    ComPtr<ID3D12Device> device;
    if (HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)); FAILED(hr))
        return dx12_error(hr, "D3D12CreateDevice failed");

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (HRESULT hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)); FAILED(hr))
        return dx12_error(hr, "ID3D12Device::CreateCommandQueue failed");

    return context_handle(std::make_shared<dx12_context>(cc::move(factory), cc::move(device), cc::move(queue)));
}
} // namespace sg
