// Runs shader.hlsl once and prints what its three products of zero and infinity came back as.
//
// Standalone: D3D12, DXGI and the C++ standard library, nothing else.
// The shader is DXIL that run.py compiled with the Windows SDK's dxc, with -Gis or without.
//
//     repro.exe <cs.cso> [--warp]
//
// Prints one line, `0*inf=<bits> inf*0=<bits> (1/0)*0=<bits>`, then the adapter's name.
// Exit codes: 0 every product is a NaN, 1 one is not, 2 setup failed.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT value_count = 5;
constexpr UINT64 bytes = value_count * sizeof(float);

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

std::uint32_t bits_of(float f)
{
    auto u = std::uint32_t{};
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: repro.exe <cs.cso> [--warp]\n");
        return 2;
    }
    auto const shader = read_file(argv[1]);
    auto const warp = argc > 2 && std::strcmp(argv[2], "--warp") == 0;
    if (shader.empty())
    {
        std::printf("could not read the shader\n");
        return 2;
    }

    ComPtr<IDXGIFactory6> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    if (warp)
        CHECK_HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    else
        CHECK_HR(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    auto desc = DXGI_ADAPTER_DESC1{};
    adapter->GetDesc1(&desc);

    ComPtr<ID3D12Device> device;
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));

    // one root UAV, the structured buffer at u0
    auto parameter = D3D12_ROOT_PARAMETER{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor.ShaderRegister = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    auto root_desc = D3D12_ROOT_SIGNATURE_DESC{};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
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
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> values;
    ComPtr<ID3D12Resource> readback;
    auto const plain = buffer_desc(bytes, D3D12_RESOURCE_FLAG_NONE);
    auto const writable = buffer_desc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    CHECK_HR(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &plain,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
    CHECK_HR(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &writable,
                                             D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&values)));
    CHECK_HR(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &plain,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    {
        float initial[value_count] = {0.0f, std::numeric_limits<float>::infinity(), 1.0f, 1.0f, 1.0f};
        void* mapped = nullptr;
        CHECK_HR(upload->Map(0, nullptr, &mapped));
        std::memcpy(mapped, initial, sizeof(initial));
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

    auto to_copy = transition(values.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &to_copy);
    list->CopyResource(values.Get(), upload.Get());
    auto to_uav = transition(values.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &to_uav);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootUnorderedAccessView(0, values->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    auto to_source = transition(values.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_source);
    list->CopyResource(readback.Get(), values.Get());
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

    float got[value_count] = {};
    void* mapped = nullptr;
    CHECK_HR(readback->Map(0, nullptr, &mapped));
    std::memcpy(got, mapped, sizeof(got));
    readback->Unmap(0, nullptr);

    std::printf("0*inf=0x%08x inf*0=0x%08x (1/0)*0=0x%08x | %ls\n", bits_of(got[2]), bits_of(got[3]), bits_of(got[4]),
                desc.Description);
    return std::isnan(got[2]) && std::isnan(got[3]) && std::isnan(got[4]) ? 0 : 1;
}
