// A cached PSO blob restored with the wrong static sampler.
//
// One compute shader samples a texture through a root-signature static sampler, at coordinates that fall outside
// 0..1 for half the texture, so a clamping sampler and a repeating one write different texels.
//
//   repro store <blob>           builds the clamping PSO fresh and writes its GetCachedBlob to <blob>
//   repro restore <blob>         builds the clamping PSO from <blob> and checks it clamps
//   repro restore <blob> twin    builds the repeating twin fresh first, then does the same
//   repro restore <blob> same    builds a clamping twin fresh first: the control, since its sampler is the blob's own
//   ... --warp                   on the WARP software adapter instead of the high-performance one
//
// Every run prints one line and exits 0 when the pipeline sampled as its root signature says, 1 when it did not.
// Standalone: D3D12, DXGI and the system shader compiler, nothing else.

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr int extent = 16;

constexpr char const* shader_source = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState smp : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // shifted by half the texture, so the first half of each axis samples outside it
    float2 uv = (float2(id.xy) + 0.5) / 16.0 - 0.5;
    dst[id.xy] = src.SampleLevel(smp, uv, 0);
}
)";

void check(HRESULT hr, char const* what)
{
    if (FAILED(hr))
    {
        std::printf("FAILED %s: 0x%08lx\n", what, static_cast<unsigned long>(hr));
        std::exit(2);
    }
}

ComPtr<ID3D12RootSignature> root_signature(ID3D12Device* device, D3D12_TEXTURE_ADDRESS_MODE address)
{
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER table = {};
    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    table.DescriptorTable = {2, ranges};
    table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = address;
    sampler.AddressV = address;
    sampler.AddressW = address;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1;
    desc.pParameters = &table;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;

    ComPtr<ID3DBlob> blob, error;
    check(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error), "serialize root signature");
    ComPtr<ID3D12RootSignature> rs;
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)),
          "create root signature");
    return rs;
}

struct pipeline
{
    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
    bool used_blob = false;
};

pipeline build(ID3D12Device* device, ID3DBlob* code, D3D12_TEXTURE_ADDRESS_MODE address, std::vector<char> const* blob)
{
    auto p = pipeline{.rs = root_signature(device, address)};
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = p.rs.Get();
    desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    if (blob != nullptr)
        desc.CachedPSO = {blob->data(), blob->size()};
    HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&p.pso));
    p.used_blob = SUCCEEDED(hr) && blob != nullptr;
    if (FAILED(hr) && blob != nullptr)
    {
        desc.CachedPSO = {};
        hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&p.pso));
    }
    check(hr, "create pipeline");
    return p;
}

/// Runs `p` over a 16 x 16 pattern and counts the texels that differ from what a clamping sampler reads.
int clamp_mismatches(ID3D12Device* device, pipeline const& p)
{
    auto texel = [](int x, int y) { return static_cast<unsigned>(((y * extent + x) * 37 + 11) & 0xff) | 0xff000000u; };

    D3D12_HEAP_PROPERTIES default_heap = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_HEAP_PROPERTIES upload_heap = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_HEAP_PROPERTIES readback_heap = {D3D12_HEAP_TYPE_READBACK};

    D3D12_RESOURCE_DESC tex = {};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = extent;
    tex.Height = extent;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex.SampleDesc = {1, 0};
    ComPtr<ID3D12Resource> src, dst;
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&src)),
          "create source");
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          nullptr, IID_PPV_ARGS(&dst)),
          "create target");

    // Rows of 16 rgba8 texels are 64 bytes, and a copy's rows are 256 apart.
    constexpr UINT pitch = 256;
    D3D12_RESOURCE_DESC buf = {};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = pitch * extent;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc = {1, 0};
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload, readback;
    check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&upload)),
          "create upload");
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&readback)),
          "create readback");
    unsigned char* mapped = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map upload");
    for (int y = 0; y < extent; ++y)
        for (int x = 0; x < extent; ++x)
        {
            auto const v = texel(x, y);
            std::memcpy(mapped + y * pitch + x * 4, &v, 4);
        }
    upload->Unmap(0, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "create heap");
    auto const step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateShaderResourceView(src.Get(), nullptr, cpu);
    cpu.ptr += step;
    device->CreateUnorderedAccessView(dst.Get(), nullptr, nullptr, cpu);

    D3D12_COMMAND_QUEUE_DESC queue_desc = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "create queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "create allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
          "create list");

    D3D12_TEXTURE_COPY_LOCATION tex_loc = {};
    tex_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION buf_loc = {};
    buf_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    buf_loc.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, extent, extent, 1, pitch};

    tex_loc.pResource = src.Get();
    buf_loc.pResource = upload.Get();
    list->CopyTextureRegion(&tex_loc, 0, 0, 0, &buf_loc, nullptr);
    D3D12_RESOURCE_BARRIER to_read = {};
    to_read.Transition = {src.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1, &to_read);

    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(p.rs.Get());
    list->SetPipelineState(p.pso.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(extent / 8, extent / 8, 1);

    D3D12_RESOURCE_BARRIER to_copy = {};
    to_copy.Transition = {dst.Get(), 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &to_copy);
    tex_loc.pResource = dst.Get();
    buf_loc.pResource = readback.Get();
    list->CopyTextureRegion(&buf_loc, 0, 0, 0, &tex_loc, nullptr);
    check(list->Close(), "close list");

    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "create fence");
    check(queue->Signal(fence.Get(), 1), "signal");
    auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(fence->SetEventOnCompletion(1, event), "wait");
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);

    unsigned char* back = nullptr;
    check(readback->Map(0, nullptr, reinterpret_cast<void**>(&back)), "map readback");
    auto mismatches = 0;
    for (int y = 0; y < extent; ++y)
        for (int x = 0; x < extent; ++x)
        {
            auto const from_x = x - extent / 2 < 0 ? 0 : x - extent / 2;
            auto const from_y = y - extent / 2 < 0 ? 0 : y - extent / 2;
            unsigned got = 0;
            std::memcpy(&got, back + y * pitch + x * 4, 4);
            mismatches += got != texel(from_x, from_y) ? 1 : 0;
        }
    readback->Unmap(0, nullptr);
    return mismatches;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: repro store|restore <blob> [twin]\n");
        return 2;
    }
    auto const mode = std::strcmp(argv[1], "store") == 0 ? 0 : 1;
    auto twin_first = false;
    auto same_twin = false;
    auto warp = false;
    for (int i = 3; i < argc; ++i)
    {
        twin_first = twin_first || std::strcmp(argv[i], "twin") == 0;
        same_twin = same_twin || std::strcmp(argv[i], "same") == 0;
        warp = warp || std::strcmp(argv[i], "--warp") == 0;
    }

    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "create factory");
    ComPtr<IDXGIAdapter1> adapter;
    if (warp)
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "pick WARP");
    else
        check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
              "pick adapter");
    DXGI_ADAPTER_DESC1 info = {};
    adapter->GetDesc1(&info);
    LARGE_INTEGER umd = {};
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "create device");

    ComPtr<ID3DBlob> code, errors;
    check(D3DCompile(shader_source, std::strlen(shader_source), "repro", nullptr, nullptr, "main", "cs_5_1", 0, 0, &code,
                     &errors),
          "compile shader");

    std::printf("%ls, driver %u.%u.%u.%u: ", info.Description, HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                HIWORD(umd.LowPart), LOWORD(umd.LowPart));
    if (mode == 0)
    {
        auto const fresh = build(device.Get(), code.Get(), D3D12_TEXTURE_ADDRESS_MODE_CLAMP, nullptr);
        ComPtr<ID3DBlob> blob;
        check(fresh.pso->GetCachedBlob(&blob), "get cached blob");
        auto* file = std::fopen(argv[2], "wb");
        if (file == nullptr)
            return 2;
        std::fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), file);
        std::fclose(file);
        auto const mismatches = clamp_mismatches(device.Get(), fresh);
        std::printf("stored %zu bytes; the fresh clamping pipeline has %d wrong texels\n", blob->GetBufferSize(),
                    mismatches);
        return mismatches == 0 ? 0 : 1;
    }

    auto blob = std::vector<char>();
    if (auto* file = std::fopen(argv[2], "rb"); file != nullptr)
    {
        char buffer[4096];
        for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;)
            blob.insert(blob.end(), buffer, buffer + n);
        std::fclose(file);
    }
    else
        return 2;

    pipeline twin;
    if (twin_first || same_twin)
        twin = build(device.Get(), code.Get(),
                     same_twin ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP, nullptr);
    auto const restored = build(device.Get(), code.Get(), D3D12_TEXTURE_ADDRESS_MODE_CLAMP, &blob);
    auto const mismatches = clamp_mismatches(device.Get(), restored);
    auto const which = same_twin ? "clamping twin built first" : twin_first ? "repeating twin built first" : "no twin";
    std::printf("%s, restored %s the blob, %d wrong texels\n", which, restored.used_blob ? "from" : "without",
                mismatches);
    return mismatches == 0 ? 0 : 1;
}
