// Runs shader.hlsl once on D3D12, 256 invocations, and prints the verdict on its round trips through a half.
//
// Standalone: D3D12, DXGI and the C++ standard library, nothing else.
// The shader is DXIL that run.py compiled with the Windows SDK's dxc and -enable-16bit-types, with -Gis or without.
//
//     repro.exe <cs.cso> [--warp]
//
// Prints one line, repro-judge.hh's.
// Exit codes: 0 every round trip is a half, 1 one is not, 2 setup failed or the adapter has no native 16-bit
// shader operations.

#include "repro-judge.hh"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace
{
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

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type)
{
    auto h = D3D12_HEAP_PROPERTIES{};
    h.Type = type;
    return h;
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
    char name[128] = {};
    std::snprintf(name, sizeof(name), "%ls", desc.Description);

    ComPtr<ID3D12Device> device;
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    auto options4 = D3D12_FEATURE_DATA_D3D12_OPTIONS4{};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4)));
    if (!options4.Native16BitShaderOpsSupported)
    {
        std::printf("the adapter has no native 16-bit shader operations | %s\n", name);
        return 2;
    }

    // a root SRV at t0, the input, and a root UAV at u1, the output
    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[0].Descriptor.ShaderRegister = 0;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[1].Descriptor.ShaderRegister = 1;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
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

    auto const in = judge::corpus_inputs();
    auto const input_bytes = UINT64(in.size() * sizeof(float));
    auto const output_bytes = UINT64(judge::outputs * sizeof(float));
    auto const upload_heap = heap(D3D12_HEAP_TYPE_UPLOAD);
    auto const default_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
    auto const readback_heap = heap(D3D12_HEAP_TYPE_READBACK);
    ComPtr<ID3D12Resource> input;
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12Resource> readback;
    auto const input_desc = buffer_desc(input_bytes, D3D12_RESOURCE_FLAG_NONE);
    auto const output_desc = buffer_desc(output_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto const readback_desc = buffer_desc(output_bytes, D3D12_RESOURCE_FLAG_NONE);
    // the shader reads the input straight from the upload heap
    CHECK_HR(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &input_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&input)));
    CHECK_HR(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output)));
    CHECK_HR(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    {
        void* mapped = nullptr;
        CHECK_HR(input->Map(0, nullptr, &mapped));
        std::memcpy(mapped, in.data(), input_bytes);
        input->Unmap(0, nullptr);
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
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootShaderResourceView(0, input->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
    list->Dispatch(judge::inputs / 64, 1, 1);
    auto to_source = transition(output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_source);
    list->CopyResource(readback.Get(), output.Get());
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

    auto got = std::vector<float>(judge::outputs);
    void* mapped = nullptr;
    CHECK_HR(readback->Map(0, nullptr, &mapped));
    std::memcpy(got.data(), mapped, output_bytes);
    readback->Unmap(0, nullptr);
    return judge::judged(in, got.data(), name) ? 0 : 1;
}
