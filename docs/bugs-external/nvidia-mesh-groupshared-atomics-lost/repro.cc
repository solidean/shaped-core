// Runs one workgroup of a mesh or compute shader container and prints the four words it writes to u0.
// usage: repro.exe <container> <ms|cs> [--vendor <hex PCI id, default 10de (NVIDIA)> | --warp] [--expect <hex>] [--at <word>]
// Exits 0 when every word is the expected one (0xf by default), and 1 when one is not.
// Exits 2 without an adapter, a device, mesh shaders or a pipeline, or with the device removed.
// --warp is there for completeness: the in-box WARP 10.0.26200 removes the device on every mesh pipeline, this one too.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace
{
// a pipeline state stream subobject: the type, then the value, each pointer-aligned as D3D12 reads them
template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, class T>
struct alignas(void*) subobject
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value;
};

struct mesh_stream
{
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> samples;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
};

D3D12_RESOURCE_DESC buffer_desc(UINT64 size, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC d = {};
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
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3)
        return std::printf("usage: repro.exe <container> <ms|cs> [--vendor <hex> | --warp] [--expect <hex>] [--at <word>]\n"), 2;
    bool const is_mesh = !std::strcmp(argv[2], "ms");
    auto vendor = 0x10deu;
    auto warp = false;
    auto expected = 0xfu;
    auto first = 0u; // the word the four results start at, past what else the shader writes to u0
    for (int i = 3; i < argc; ++i)
        if (!std::strcmp(argv[i], "--warp"))
            warp = true;
        else if (!std::strcmp(argv[i], "--vendor") && i + 1 < argc)
            vendor = unsigned(std::strtoul(argv[++i], nullptr, 16));
        else if (!std::strcmp(argv[i], "--expect") && i + 1 < argc)
            expected = unsigned(std::strtoul(argv[++i], nullptr, 16));
        else if (!std::strcmp(argv[i], "--at") && i + 1 < argc)
            first = unsigned(std::strtoul(argv[++i], nullptr, 10));

    std::ifstream file(argv[1], std::ios::binary);
    auto const code = std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    if (warp)
        factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    else
        for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 desc = {};
            adapter->GetDesc1(&desc);
            if (desc.VendorId == vendor)
                break;
        }
    if (!adapter)
        return std::printf("no adapter\n"), 2;
    DXGI_ADAPTER_DESC1 adesc = {};
    adapter->GetDesc1(&adesc);
    std::printf("adapter: %ls\n", adesc.Description);

    ComPtr<ID3D12Device2> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
        return std::printf("no device\n"), 2;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7 = {};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7));
    if (is_mesh && options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
        return std::printf("no mesh shaders\n"), 2;

    // u0 as a root descriptor, which is all the shader binds
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    param.Descriptor = {0, 0};
    D3D12_ROOT_SIGNATURE_DESC const root_desc = {1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        return std::printf("no root signature\n"), 2;
    ComPtr<ID3D12RootSignature> root;
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));

    ComPtr<ID3D12PipelineState> pso;
    if (is_mesh)
    {
        D3D12_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D12_FILL_MODE_SOLID;
        raster.CullMode = D3D12_CULL_MODE_NONE;
        raster.DepthClipEnable = TRUE;
        auto stream = mesh_stream{};
        stream.root.value = root.Get();
        stream.ms.value = {code.data(), code.size()};
        stream.rasterizer.value = raster;
        stream.samples.value = {1, 0};
        stream.sample_mask.value = UINT_MAX;
        D3D12_PIPELINE_STATE_STREAM_DESC const desc = {sizeof(stream), &stream};
        if (HRESULT const hr = device->CreatePipelineState(&desc, IID_PPV_ARGS(&pso)); FAILED(hr))
            return std::printf("CreatePipelineState: hr=0x%08X\n", unsigned(hr)), 2;
    }
    else
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root.Get();
        desc.CS = {code.data(), code.size()};
        if (HRESULT const hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)); FAILED(hr))
            return std::printf("CreateComputePipelineState: hr=0x%08X\n", unsigned(hr)), 2;
    }

    D3D12_HEAP_PROPERTIES const on_gpu = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_HEAP_PROPERTIES const readable = {D3D12_HEAP_TYPE_READBACK};
    ComPtr<ID3D12Resource> words, readback;
    auto const uav_desc = buffer_desc(256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto const rb_desc = buffer_desc(256, D3D12_RESOURCE_FLAG_NONE);
    device->CreateCommittedResource(&on_gpu, D3D12_HEAP_FLAG_NONE, &uav_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                    nullptr, IID_PPV_ARGS(&words));
    device->CreateCommittedResource(&readable, D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                    IID_PPV_ARGS(&readback));

    D3D12_COMMAND_QUEUE_DESC const queue_desc = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    ComPtr<ID3D12CommandAllocator> allocator;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    ComPtr<ID3D12GraphicsCommandList6> list;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(), IID_PPV_ARGS(&list));

    if (is_mesh)
    {
        D3D12_VIEWPORT const viewport = {0, 0, 4, 4, 0, 1};
        D3D12_RECT const scissor = {0, 0, 4, 4};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(root.Get());
        list->SetGraphicsRootUnorderedAccessView(0, words->GetGPUVirtualAddress());
        list->DispatchMesh(1, 1, 1);
    }
    else
    {
        list->SetComputeRootSignature(root.Get());
        list->SetComputeRootUnorderedAccessView(0, words->GetGPUVirtualAddress());
        list->Dispatch(1, 1, 1);
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {words.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    list->CopyResource(readback.Get(), words.Get());
    list->Close();
    ID3D12CommandList* const lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    queue->Signal(fence.Get(), 1);
    fence->SetEventOnCompletion(1, nullptr);

    unsigned* seen = nullptr;
    D3D12_RANGE const range = {0, 256};
    if (HRESULT const hr = readback->Map(0, &range, reinterpret_cast<void**>(&seen)); FAILED(hr) || !seen)
        return std::printf("Map: hr=0x%08X, device removed reason 0x%08X\n", unsigned(hr),
                           unsigned(device->GetDeviceRemovedReason())),
               2;
    auto const* const at = seen + first;
    std::printf("seen: %x %x %x %x\n", at[0], at[1], at[2], at[3]);
    auto const ok = at[0] == expected && at[1] == expected && at[2] == expected && at[3] == expected;
    readback->Unmap(0, nullptr);
    return ok ? 0 : 1;
}
