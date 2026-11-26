// Runs one of the DXIL containers beside it once on one adapter and prints what the four values came back as.
//
// Standalone: D3D12, DXGI and the C++ standard library, nothing else.
//
//     repro.exe <shader.dxil> (--adapter <substring> | --warp)
//
// Prints one line, `values {a, b, c, d} | <adapter>`, where {2, 104, 3, 308} is right.
// Exit codes: 0 right, 1 wrong, 2 setup failed, 3 no hardware adapter's name holds the substring.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT value_count = 4;
constexpr std::int32_t expected[value_count] = {2, 104, 3, 308};

#define CHECK_HR(expr)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (HRESULT hr_ = (expr); FAILED(hr_))                                                                         \
        {                                                                                                              \
            std::printf("FAILED %s: 0x%08lx\n", #expr, static_cast<unsigned long>(hr_));                              \
            return 2;                                                                                                  \
        }                                                                                                              \
    } while (false)

std::vector<char> read_file(char const* path)
{
    auto f = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 size, D3D12_RESOURCE_FLAGS flags)
{
    auto d = D3D12_RESOURCE_DESC{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    auto b = D3D12_RESOURCE_BARRIER{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

/// The first hardware adapter whose name holds `name`, compared case-insensitively.
ComPtr<IDXGIAdapter1> find_adapter(IDXGIFactory6* factory, char const* name)
{
    wchar_t wide[128] = {};
    for (int c = 0; c < 127 && name[c]; ++c)
        wide[c] = wchar_t(static_cast<unsigned char>(name[c]));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        auto desc = DXGI_ADAPTER_DESC1{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;
        wchar_t lowered[128] = {};
        for (int c = 0; c < 127 && desc.Description[c]; ++c)
            lowered[c] = towlower(desc.Description[c]);
        for (int c = 0; wide[c]; ++c)
            wide[c] = towlower(wide[c]);
        if (std::wcsstr(lowered, wide))
            return adapter;
    }
    return nullptr;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: repro.exe <cs.cso> (--adapter <substring> | --warp)\n");
        return 2;
    }
    auto const shader = read_file(argv[1]);
    if (shader.empty())
    {
        std::printf("could not read the shader\n");
        return 2;
    }

    ComPtr<IDXGIFactory6> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    if (std::strcmp(argv[2], "--warp") == 0)
        CHECK_HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    else if (std::strcmp(argv[2], "--adapter") == 0 && argc > 3)
        adapter = find_adapter(factory.Get(), argv[3]);
    if (!adapter)
    {
        std::printf("no hardware adapter matches\n");
        return 3;
    }
    auto desc = DXGI_ADAPTER_DESC1{};
    adapter->GetDesc1(&desc);

    ComPtr<ID3D12Device> device;
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));

    // the shader's two bindings: the values at u0, and the threads a dispatch covers at b2 of space 12, as sg binds them
    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[0].Descriptor = {.ShaderRegister = 0, .RegisterSpace = 0};
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {.ShaderRegister = 2, .RegisterSpace = 12, .Num32BitValues = 4};
    for (auto& p : parameters)
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    auto root_desc = D3D12_ROOT_SIGNATURE_DESC{};
    root_desc.NumParameters = 2;
    root_desc.pParameters = parameters;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    CHECK_HR(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ComPtr<ID3D12RootSignature> root;
    CHECK_HR(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                         IID_PPV_ARGS(&root)));

    auto pso_desc = D3D12_COMPUTE_PIPELINE_STATE_DESC{};
    pso_desc.pRootSignature = root.Get();
    pso_desc.CS = {shader.data(), shader.size()};
    ComPtr<ID3D12PipelineState> pso;
    CHECK_HR(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

    auto const heap = [](D3D12_HEAP_TYPE type)
    {
        auto h = D3D12_HEAP_PROPERTIES{};
        h.Type = type;
        return h;
    };
    auto const upload_heap = heap(D3D12_HEAP_TYPE_UPLOAD);
    auto const default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
    auto const readback_heap = heap(D3D12_HEAP_TYPE_READBACK);

    // one upload and one readback buffer beside the values
    constexpr UINT64 value_bytes = value_count * sizeof(std::int32_t);
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Resource> values;
    auto const staging = buffer_desc(value_bytes, D3D12_RESOURCE_FLAG_NONE);
    auto const value_desc = buffer_desc(value_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CHECK_HR(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &staging,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    CHECK_HR(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &staging,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    CHECK_HR(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &value_desc,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&values)));
    {
        std::int32_t const initial[value_count] = {1, 2, 3, 4};
        void* mapped = nullptr;
        CHECK_HR(upload->Map(0, nullptr, &mapped));
        std::memcpy(mapped, initial, value_bytes);
        upload->Unmap(0, nullptr);
    }

    auto queue_desc = D3D12_COMMAND_QUEUE_DESC{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    CHECK_HR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    CHECK_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                       IID_PPV_ARGS(&list)));

    auto const to_copy = transition(values.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &to_copy);
    list->CopyBufferRegion(values.Get(), 0, upload.Get(), 0, value_bytes);
    auto const to_uav = transition(values.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &to_uav);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootUnorderedAccessView(0, values->GetGPUVirtualAddress());
    std::uint32_t const grid[4] = {value_count, 1, 1, 0}; // the threads the dispatch covers
    list->SetComputeRoot32BitConstants(1, 4, grid, 0);
    list->Dispatch(1, 1, 1);
    auto const to_source
        = transition(values.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_source);
    list->CopyBufferRegion(readback.Get(), 0, values.Get(), 0, value_bytes);
    CHECK_HR(list->Close());
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    CHECK_HR(queue->Signal(fence.Get(), 1));
    auto const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK_HR(fence->SetEventOnCompletion(1, event));
    if (WaitForSingleObject(event, 10000) != WAIT_OBJECT_0)
    {
        std::printf("the dispatch did not finish within 10 s\n");
        return 2;
    }
    CloseHandle(event);

    std::int32_t got[value_count] = {};
    void* mapped = nullptr;
    CHECK_HR(readback->Map(0, nullptr, &mapped));
    std::memcpy(got, mapped, value_bytes);
    readback->Unmap(0, nullptr);

    std::printf("values {%d, %d, %d, %d} | %ls\n", got[0], got[1], got[2], got[3], desc.Description);
    return std::memcmp(got, expected, sizeof(got)) == 0 ? 0 : 1;
}
