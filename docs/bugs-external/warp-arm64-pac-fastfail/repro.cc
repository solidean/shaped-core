// WARP on Windows on ARM: a fastfail after a bounds-checked read past a buffer's end.
//
// D3D12, DXGI and the system shader compiler (FXC) only; see readme.md for the matrix this drives.
//
//   repro.exe adapters                 every adapter DXGI enumerates, and whether it creates a D3D12 device
//   repro.exe <ps|cs> <offset> [--warp]  one shader reads in[i + offset] for i in [0, 64) and writes it out
//
// `in` holds 64 floats, 1 to 64, behind a descriptor that bounds it at 64 elements.
// D3D12 defines a read past those bounds as 0, so every output is either `i + offset + 1` or 0.
// Exit 0 when every output is right, 1 when one is wrong, 2 when the setup failed.
// A crash shows as the process's own exit code, which run.py reads.

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT k_count = 64;

// A full-screen triangle; the pixel shader runs once per texel of a 64 x 1 target.
char const* const k_vs = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";

char const* const k_ps = R"(
cbuffer constants : register(b0) { uint offset; };
StructuredBuffer<float> source : register(t0);
float main(float4 position : SV_Position) : SV_Target
{
    return source[uint(position.x) + offset];
}
)";

char const* const k_cs = R"(
cbuffer constants : register(b0) { uint offset; };
StructuredBuffer<float> source : register(t0);
RWStructuredBuffer<float> target : register(u0);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    target[id.x] = source[id.x + offset];
}
)";

[[noreturn]] void fail(char const* what, HRESULT hr)
{
    std::printf("setup failed: %s (hr 0x%08lx)\n", what, static_cast<unsigned long>(hr));
    std::exit(2);
}

void check(HRESULT hr, char const* what)
{
    if (FAILED(hr))
        fail(what, hr);
}

ComPtr<ID3DBlob> compile(char const* source, char const* profile)
{
    ComPtr<ID3DBlob> code = nullptr;
    ComPtr<ID3DBlob> errors = nullptr;
    HRESULT const hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, "main", profile, 0, 0,
                                  code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr))
    {
        if (errors != nullptr)
            std::printf("%s\n", static_cast<char const*>(errors->GetBufferPointer()));
        fail("D3DCompile", hr);
    }
    return code;
}

std::string narrow(wchar_t const* text)
{
    std::string out;
    for (wchar_t const* c = text; *c != L'\0'; ++c)
        out.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    return out;
}

int list_adapters(IDXGIFactory4* factory)
{
    ComPtr<IDXGIAdapter1> adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        bool const software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        bool const d3d12 = SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
        std::printf("adapter %u: \"%s\" vendor 0x%04x device 0x%04x software-flag %d d3d12 %d\n", i,
                    narrow(desc.Description).c_str(), desc.VendorId, desc.DeviceId, software ? 1 : 0, d3d12 ? 1 : 0);
    }
    return 0;
}

/// The first adapter without DXGI's software flag that creates a device: what an application asking for "the GPU" gets.
ComPtr<IDXGIAdapter1> first_non_software_adapter(IDXGIFactory4* factory)
{
    ComPtr<IDXGIAdapter1> adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            return adapter;
    }
    return nullptr;
}

ComPtr<ID3D12Resource> create_buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, UINT64 bytes, D3D12_RESOURCE_FLAGS flags,
                                     D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES const props = {.Type = heap};
    D3D12_RESOURCE_DESC const desc = {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                      .Width = bytes,
                                      .Height = 1,
                                      .DepthOrArraySize = 1,
                                      .MipLevels = 1,
                                      .SampleDesc = {.Count = 1},
                                      .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                      .Flags = flags};
    ComPtr<ID3D12Resource> buffer = nullptr;
    check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer)),
          "CreateCommittedResource (buffer)");
    return buffer;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    barrier.Transition = {.pResource = resource,
                          .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          .StateBefore = from,
                          .StateAfter = to};
    list->ResourceBarrier(1, &barrier);
}
} // namespace

int main(int argc, char** argv)
{
    ComPtr<IDXGIFactory4> factory = nullptr;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    if (argc >= 2 && std::strcmp(argv[1], "adapters") == 0)
        return list_adapters(factory.Get());
    if (argc < 3)
    {
        std::printf("usage: repro adapters | repro <ps|cs> <offset> [--warp]\n");
        return 2;
    }
    bool const pixel = std::strcmp(argv[1], "ps") == 0;
    UINT const offset = static_cast<UINT>(std::strtoul(argv[2], nullptr, 10));
    bool const warp = argc >= 4 && std::strcmp(argv[3], "--warp") == 0;

    ComPtr<IDXGIAdapter1> adapter = nullptr;
    if (warp)
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
    else
        adapter = first_non_software_adapter(factory.Get());
    if (adapter == nullptr)
        fail("no adapter without the software flag creates a D3D12 device", E_FAIL);

    ComPtr<ID3D12Device> device = nullptr;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC const queue_desc = {.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue = nullptr;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12CommandAllocator> allocator = nullptr;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    ComPtr<ID3D12GraphicsCommandList> list = nullptr;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
          "CreateCommandList");

    // The source lives in an upload heap, which a buffer SRV may read directly.
    auto const source = create_buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, k_count * sizeof(float), D3D12_RESOURCE_FLAG_NONE,
                                      D3D12_RESOURCE_STATE_GENERIC_READ);
    float* mapped = nullptr;
    check(source->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map (source)");
    for (UINT i = 0; i < k_count; ++i)
        mapped[i] = static_cast<float>(i + 1);
    source->Unmap(0, nullptr);

    // A descriptor, not a root SRV: only a descriptor carries the bounds that make a read past the end defined.
    D3D12_DESCRIPTOR_HEAP_DESC const heap_desc = {.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                  .NumDescriptors = 2,
                                                  .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> heap = nullptr;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
    UINT const increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE const srv_cpu = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE const uav_cpu = {.ptr = srv_cpu.ptr + increment};
    D3D12_GPU_DESCRIPTOR_HANDLE const srv_gpu = heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE const uav_gpu = {.ptr = srv_gpu.ptr + increment};

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {.Format = DXGI_FORMAT_UNKNOWN,
                                           .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                                           .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.Buffer = {.FirstElement = 0, .NumElements = k_count, .StructureByteStride = sizeof(float)};
    device->CreateShaderResourceView(source.Get(), &srv, srv_cpu);

    D3D12_DESCRIPTOR_RANGE const srv_range = {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV, .NumDescriptors = 1};
    D3D12_DESCRIPTOR_RANGE const uav_range = {.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV, .NumDescriptors = 1};
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {.ShaderRegister = 0, .RegisterSpace = 0, .Num32BitValues = 1};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {.NumDescriptorRanges = 1, .pDescriptorRanges = &srv_range};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = {.NumDescriptorRanges = 1, .pDescriptorRanges = &uav_range};
    D3D12_ROOT_SIGNATURE_DESC const root_desc = {.NumParameters = pixel ? 2u : 3u, .pParameters = params};
    ComPtr<ID3DBlob> root_blob = nullptr;
    ComPtr<ID3DBlob> root_errors = nullptr;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, root_blob.GetAddressOf(),
                                      root_errors.GetAddressOf()),
          "D3D12SerializeRootSignature");
    ComPtr<ID3D12RootSignature> root = nullptr;
    check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&root)),
          "CreateRootSignature");

    // What the shader wrote, copied to a buffer the CPU reads; 64 floats is exactly one 256-byte row.
    auto const readback = create_buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, k_count * sizeof(float),
                                        D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12PipelineState> pipeline = nullptr;
    ComPtr<ID3D12Resource> target = nullptr;
    ComPtr<ID3D12DescriptorHeap> rtv_heap = nullptr;

    ID3D12DescriptorHeap* const heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);

    if (pixel)
    {
        auto const vs = compile(k_vs, "vs_5_0");
        auto const ps = compile(k_ps, "ps_5_0");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {.pRootSignature = root.Get(),
                                                  .VS = {vs->GetBufferPointer(), vs->GetBufferSize()},
                                                  .PS = {ps->GetBufferPointer(), ps->GetBufferSize()},
                                                  .SampleMask = UINT_MAX,
                                                  .PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE,
                                                  .NumRenderTargets = 1,
                                                  .SampleDesc = {.Count = 1}};
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.RasterizerState = {.FillMode = D3D12_FILL_MODE_SOLID, .CullMode = D3D12_CULL_MODE_NONE, .DepthClipEnable = TRUE};
        pso.RTVFormats[0] = DXGI_FORMAT_R32_FLOAT;
        check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)), "CreateGraphicsPipelineState");

        D3D12_HEAP_PROPERTIES const props = {.Type = D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC const tex = {.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                         .Width = k_count,
                                         .Height = 1,
                                         .DepthOrArraySize = 1,
                                         .MipLevels = 1,
                                         .Format = DXGI_FORMAT_R32_FLOAT,
                                         .SampleDesc = {.Count = 1},
                                         .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
        check(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                              IID_PPV_ARGS(&target)),
              "CreateCommittedResource (target)");
        D3D12_DESCRIPTOR_HEAP_DESC const rtv_desc = {.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1};
        check(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)), "CreateDescriptorHeap (rtv)");
        D3D12_CPU_DESCRIPTOR_HANDLE const rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target.Get(), nullptr, rtv);

        D3D12_VIEWPORT const viewport = {.Width = static_cast<float>(k_count), .Height = 1.0f, .MaxDepth = 1.0f};
        D3D12_RECT const scissor = {.right = static_cast<LONG>(k_count), .bottom = 1};
        list->SetPipelineState(pipeline.Get());
        list->SetGraphicsRootSignature(root.Get());
        list->SetGraphicsRoot32BitConstant(0, offset, 0);
        list->SetGraphicsRootDescriptorTable(1, srv_gpu);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);

        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from = {.pResource = target.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        from.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION to = {.pResource = readback.Get(), .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        to.PlacedFootprint = {.Offset = 0,
                              .Footprint = {.Format = DXGI_FORMAT_R32_FLOAT,
                                            .Width = k_count,
                                            .Height = 1,
                                            .Depth = 1,
                                            .RowPitch = k_count * sizeof(float)}};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    else
    {
        auto const cs = compile(k_cs, "cs_5_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC const pso = {.pRootSignature = root.Get(),
                                                       .CS = {cs->GetBufferPointer(), cs->GetBufferSize()}};
        check(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)), "CreateComputePipelineState");

        target = create_buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, k_count * sizeof(float),
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {.Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D12_UAV_DIMENSION_BUFFER};
        uav.Buffer = {.FirstElement = 0, .NumElements = k_count, .StructureByteStride = sizeof(float)};
        device->CreateUnorderedAccessView(target.Get(), nullptr, &uav, uav_cpu);

        list->SetPipelineState(pipeline.Get());
        list->SetComputeRootSignature(root.Get());
        list->SetComputeRoot32BitConstant(0, offset, 0);
        list->SetComputeRootDescriptorTable(1, srv_gpu);
        list->SetComputeRootDescriptorTable(2, uav_gpu);
        list->Dispatch(1, 1, 1);

        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, k_count * sizeof(float));
    }

    check(list->Close(), "Close");
    ID3D12CommandList* const lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence = nullptr;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    check(queue->Signal(fence.Get(), 1), "Signal");
    check(fence->SetEventOnCompletion(1, nullptr), "SetEventOnCompletion"); // a null event blocks until it is reached

    float* result = nullptr;
    D3D12_RANGE const whole = {.Begin = 0, .End = k_count * sizeof(float)};
    check(readback->Map(0, &whole, reinterpret_cast<void**>(&result)), "Map (readback)");
    int wrong = 0;
    for (UINT i = 0; i < k_count; ++i)
    {
        float const expected = i + offset < k_count ? static_cast<float>(i + offset + 1) : 0.0f;
        if (result[i] != expected)
        {
            if (wrong == 0)
                std::printf("WRONG: output %u is %g, expected %g\n", i, result[i], expected);
            ++wrong;
        }
    }
    readback->Unmap(0, nullptr);

    DXGI_ADAPTER_DESC1 desc = {};
    adapter->GetDesc1(&desc);
    std::printf("%s offset %u on \"%s\": %s\n", pixel ? "ps" : "cs", offset, narrow(desc.Description).c_str(),
                wrong == 0 ? "correct" : "wrong");
    return wrong == 0 ? 0 : 1;
}
