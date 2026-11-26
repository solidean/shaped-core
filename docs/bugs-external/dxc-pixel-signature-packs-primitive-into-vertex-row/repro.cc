// Draws one mesh-shader group into two 8x8 RGBA32F targets and prints how many pixels it covered.
//
// Standalone: D3D12, DXGI and the C++ standard library, nothing else.
// The shaders are DXIL that run.py compiled with the Windows SDK's dxc.
//
//     repro.exe <mesh.cso> <pixel.cso> (--adapter <substring> | --warp)
//
// A pixel counts as covered where the second target's alpha is 1, which the pixel stage writes.
// Prints one line, `covered <n> of 64, second target at (1, 1) = (r, g, b, a) | <adapter>`.
// The right answer is all 64 covered, and (1.5, 3, 0, 1) at (1, 1): the second triangle's weight.
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
constexpr UINT size = 8;
constexpr UINT texel_bytes = 16;
constexpr UINT row_pitch = 256; // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, past the 128 bytes a row holds
constexpr UINT target_count = 2;

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

/// The first hardware adapter whose name holds `name`, compared case-insensitively.
ComPtr<IDXGIAdapter1> find_adapter(IDXGIFactory6* factory, char const* name)
{
    wchar_t wanted[128] = {};
    for (int c = 0; c < 127 && name[c]; ++c)
        wanted[c] = towlower(wchar_t(static_cast<unsigned char>(name[c])));
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
        if (std::wcsstr(lowered, wanted))
            return adapter;
    }
    return nullptr;
}

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, class T>
struct alignas(void*) subobject
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value = {};
};

/// A mesh-shaded pipeline: the only way to state one is a stream.
struct mesh_stream
{
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root_signature;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, D3D12_BLEND_DESC> blend;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT> sample_mask;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, D3D12_DEPTH_STENCIL_DESC> depth_stencil;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> rtv_formats;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC> sample_desc;
};
} // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::printf("usage: repro.exe <mesh.cso> <pixel.cso> (--adapter <substring> | --warp)\n");
        return 2;
    }
    auto const mesh = read_file(argv[1]);
    auto const pixel = read_file(argv[2]);
    if (mesh.empty() || pixel.empty())
    {
        std::printf("could not read a shader\n");
        return 2;
    }

    ComPtr<IDXGIFactory6> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    if (std::strcmp(argv[3], "--warp") == 0)
        CHECK_HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    else if (std::strcmp(argv[3], "--adapter") == 0 && argc > 4)
        adapter = find_adapter(factory.Get(), argv[4]);
    if (!adapter)
    {
        std::printf("no hardware adapter matches\n");
        return 3;
    }
    auto desc = DXGI_ADAPTER_DESC1{};
    adapter->GetDesc1(&desc);

    ComPtr<ID3D12Device2> device;
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    auto options7 = D3D12_FEATURE_DATA_D3D12_OPTIONS7{};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)));
    if (options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
    {
        std::printf("no mesh shaders | %ls\n", desc.Description);
        return 2;
    }

    auto root_desc = D3D12_ROOT_SIGNATURE_DESC{};
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    CHECK_HR(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error));
    ComPtr<ID3D12RootSignature> root;
    CHECK_HR(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                         IID_PPV_ARGS(&root)));

    auto stream = mesh_stream{};
    stream.root_signature.value = root.Get();
    stream.ms.value = {mesh.data(), mesh.size()};
    stream.ps.value = {pixel.data(), pixel.size()};
    stream.rasterizer.value = D3D12_RASTERIZER_DESC{.FillMode = D3D12_FILL_MODE_SOLID,
                                                    .CullMode = D3D12_CULL_MODE_NONE,
                                                    .DepthClipEnable = TRUE};
    for (UINT i = 0; i < target_count; ++i)
        stream.blend.value.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    stream.sample_mask.value = UINT_MAX;
    stream.depth_stencil.value = D3D12_DEPTH_STENCIL_DESC{.DepthEnable = FALSE, .StencilEnable = FALSE};
    stream.rtv_formats.value.NumRenderTargets = target_count;
    for (UINT i = 0; i < target_count; ++i)
        stream.rtv_formats.value.RTFormats[i] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    stream.sample_desc.value = DXGI_SAMPLE_DESC{.Count = 1};
    auto const stream_desc
        = D3D12_PIPELINE_STATE_STREAM_DESC{.SizeInBytes = sizeof(stream), .pPipelineStateSubobjectStream = &stream};
    ComPtr<ID3D12PipelineState> pso;
    CHECK_HR(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pso)));

    auto const default_heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_DEFAULT};
    auto const readback_heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_READBACK};
    auto const texture_desc = D3D12_RESOURCE_DESC{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                                  .Width = size,
                                                  .Height = size,
                                                  .DepthOrArraySize = 1,
                                                  .MipLevels = 1,
                                                  .Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
                                                  .SampleDesc = {.Count = 1},
                                                  .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    auto const readback_desc = D3D12_RESOURCE_DESC{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                                   .Width = row_pitch * size,
                                                   .Height = 1,
                                                   .DepthOrArraySize = 1,
                                                   .MipLevels = 1,
                                                   .SampleDesc = {.Count = 1},
                                                   .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    auto const clear = D3D12_CLEAR_VALUE{.Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .Color = {0, 0, 0, 0}};
    ComPtr<ID3D12Resource> targets[target_count];
    ComPtr<ID3D12Resource> readbacks[target_count];
    for (UINT i = 0; i < target_count; ++i)
    {
        CHECK_HR(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                 IID_PPV_ARGS(&targets[i])));
        CHECK_HR(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&readbacks[i])));
    }

    auto const heap_desc
        = D3D12_DESCRIPTOR_HEAP_DESC{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = target_count};
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    CHECK_HR(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap)));
    auto const rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[target_count];
    for (UINT i = 0; i < target_count; ++i)
    {
        rtvs[i] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtvs[i].ptr += i * rtv_step;
        device->CreateRenderTargetView(targets[i].Get(), nullptr, rtvs[i]);
    }

    auto queue_desc = D3D12_COMMAND_QUEUE_DESC{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK_HR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    CHECK_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList6> list;
    CHECK_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                       IID_PPV_ARGS(&list)));

    float const zero[4] = {0, 0, 0, 0};
    for (auto const& rtv : rtvs)
        list->ClearRenderTargetView(rtv, zero, 0, nullptr);
    list->OMSetRenderTargets(target_count, rtvs, FALSE, nullptr);
    auto const viewport = D3D12_VIEWPORT{.Width = float(size), .Height = float(size), .MaxDepth = 1.0f};
    auto const scissor = D3D12_RECT{.right = LONG(size), .bottom = LONG(size)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(root.Get());
    list->DispatchMesh(1, 1, 1);
    for (UINT i = 0; i < target_count; ++i)
    {
        auto barrier = D3D12_RESOURCE_BARRIER{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        barrier.Transition = {.pResource = targets[i].Get(),
                              .Subresource = 0,
                              .StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET,
                              .StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        auto const from = D3D12_TEXTURE_COPY_LOCATION{.pResource = targets[i].Get(),
                                                      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                                      .SubresourceIndex = 0};
        auto to = D3D12_TEXTURE_COPY_LOCATION{.pResource = readbacks[i].Get(),
                                              .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        to.PlacedFootprint.Footprint = {.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
                                        .Width = size,
                                        .Height = size,
                                        .Depth = 1,
                                        .RowPitch = row_pitch};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
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
        std::printf("the draw did not finish within 10 s\n");
        return 2;
    }
    CloseHandle(event);
    CHECK_HR(device->GetDeviceRemovedReason());

    char* mapped = nullptr;
    CHECK_HR(readbacks[1]->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    auto const texel = [&](UINT x, UINT y)
    {
        float v[4];
        std::memcpy(v, mapped + y * row_pitch + x * texel_bytes, sizeof(v));
        return std::vector<float>(v, v + 4);
    };
    auto covered = 0;
    for (UINT y = 0; y < size; ++y)
        for (UINT x = 0; x < size; ++x)
            covered += texel(x, y)[3] == 1.0f ? 1 : 0;
    auto const probe = texel(1, 1);
    readbacks[1]->Unmap(0, nullptr);

    std::printf("covered %d of %u, second target at (1, 1) = (%g, %g, %g, %g) | %ls\n", covered, size * size, probe[0],
                probe[1], probe[2], probe[3], desc.Description);
    auto const right = covered == int(size * size) && probe[0] == 1.5f && probe[1] == 3.0f && probe[2] == 0.0f
                    && probe[3] == 1.0f;
    return right ? 0 : 1;
}
