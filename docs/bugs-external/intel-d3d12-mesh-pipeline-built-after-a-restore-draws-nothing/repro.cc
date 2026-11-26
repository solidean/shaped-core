// Draws one full-screen triangle through a mesh-shaded pipeline, and reads a pixel back.
//
//     repro.exe <ms.cso | vs.cso> <ps.cso> <blob file> <steps> [adapter substring] [--vertex]
//
// With --vertex the first stage is a vertex stage, and the triangle is drawn as three vertices of no buffer.
// Steps run in order in this process: f builds fresh and draws, s saves the last pipeline's blob to <blob file>,
// and r builds from <blob file> and draws.
// Prints one line a draw: the pixel read, green where the triangle landed.
// Exit codes: 0 every draw landed, 1 some draw read the clear value, 2 setup failed, 3 no adapter matched.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

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
std::vector<char> read_file(char const* path)
{
    auto f = std::ifstream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

template <D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type, class T>
struct alignas(void*) subobject
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value = {};
};

struct mesh_stream
{
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*> root;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE> ps;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, D3D12_RASTERIZER_DESC> rasterizer;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, D3D12_RT_FORMAT_ARRAY> targets;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO, D3D12_CACHED_PIPELINE_STATE> cached;
};

#define CHECK_HR(expr)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        if (FAILED(expr))                                                                                              \
        {                                                                                                              \
            std::printf("FAILED %s\n", #expr);                                                                         \
            return 2;                                                                                                  \
        }                                                                                                              \
    } while (false)
} // namespace

int main(int argc, char** argv)
{
    if (argc < 5)
    {
        std::printf("usage: repro.exe <ms.cso> <ps.cso> <blob file> <steps> [adapter substring]\n");
        return 2;
    }
    auto const ms = read_file(argv[1]);
    auto const ps = read_file(argv[2]);
    wchar_t wanted[64] = L"Intel";
    auto vertex = false;
    for (int a = 5; a < argc; ++a)
    {
        if (std::strcmp(argv[a], "--vertex") == 0)
            vertex = true;
        else
            for (int c = 0; c < 63 && argv[a][c] != 0; ++c)
                wanted[c] = wchar_t(argv[a][c]), wanted[c + 1] = 0;
    }

    ComPtr<IDXGIFactory6> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    auto desc = DXGI_ADAPTER_DESC1{};
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        adapter->GetDesc1(&desc);
        if (std::wcsstr(desc.Description, wanted))
            break;
        adapter = nullptr;
    }
    if (!adapter)
    {
        std::printf("no adapter matches\n");
        return 3;
    }
    ComPtr<ID3D12Device2> device;
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));

    // one root constant nothing reads, since an empty root signature hangs a mesh stage on this driver otherwise
    auto param = D3D12_ROOT_PARAMETER{.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                                      .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL};
    param.Constants = {.ShaderRegister = 0, .RegisterSpace = 100, .Num32BitValues = 1};
    auto const root_desc = D3D12_ROOT_SIGNATURE_DESC{.NumParameters = 1, .pParameters = &param};
    ComPtr<ID3DBlob> serialized;
    CHECK_HR(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr));
    ComPtr<ID3D12RootSignature> root;
    CHECK_HR(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                         IID_PPV_ARGS(&root)));

    auto const default_heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_DEFAULT};
    auto const readback_heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_READBACK};
    auto const target_desc = D3D12_RESOURCE_DESC{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                                 .Width = 8,
                                                 .Height = 8,
                                                 .DepthOrArraySize = 1,
                                                 .MipLevels = 1,
                                                 .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                                 .SampleDesc = {1, 0},
                                                 .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> target;
    CHECK_HR(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)));
    auto const buffer_desc = D3D12_RESOURCE_DESC{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                                 .Width = 256 * 8,
                                                 .Height = 1,
                                                 .DepthOrArraySize = 1,
                                                 .MipLevels = 1,
                                                 .SampleDesc = {1, 0},
                                                 .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> readback;
    CHECK_HR(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    auto const rtv_heap_desc = D3D12_DESCRIPTOR_HEAP_DESC{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1};
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    CHECK_HR(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap)));
    auto const rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    auto const queue_desc = D3D12_COMMAND_QUEUE_DESC{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK_HR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> fence;
    CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    auto fence_value = UINT64(0);

    auto all_landed = true;
    ComPtr<ID3D12PipelineState> last;
    for (char const* step = argv[4]; *step != 0; ++step)
    {
        if (*step == 's')
        {
            ComPtr<ID3DBlob> blob;
            CHECK_HR(last->GetCachedBlob(&blob));
            std::ofstream(argv[3], std::ios::binary)
                .write(static_cast<char const*>(blob->GetBufferPointer()), std::streamsize(blob->GetBufferSize()));
            continue;
        }
        auto const restoring = *step == 'r';
        auto const blob = restoring ? read_file(argv[3]) : std::vector<char>();
        auto stream = mesh_stream{};
        stream.root.value = root.Get();
        if (vertex)
            stream.vs.value = {ms.data(), ms.size()};
        else
            stream.ms.value = {ms.data(), ms.size()};
        stream.ps.value = {ps.data(), ps.size()};
        stream.rasterizer.value
            = {.FillMode = D3D12_FILL_MODE_SOLID, .CullMode = D3D12_CULL_MODE_NONE, .DepthClipEnable = TRUE};
        stream.targets.value.NumRenderTargets = 1;
        stream.targets.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        stream.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        stream.cached.value = {blob.empty() ? nullptr : blob.data(), blob.size()};
        auto const stream_desc = D3D12_PIPELINE_STATE_STREAM_DESC{.SizeInBytes = sizeof(stream),
                                                                  .pPipelineStateSubobjectStream = &stream};
        ComPtr<ID3D12PipelineState> pso;
        if (auto const hr = device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pso)); FAILED(hr))
        {
            std::printf("%c: create failed 0x%08lx\n", *step, static_cast<unsigned long>(hr));
            return 2;
        }
        last = pso;

        ComPtr<ID3D12CommandAllocator> allocator;
        CHECK_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList6> list;
        CHECK_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                           IID_PPV_ARGS(&list)));
        float const clear[4] = {0, 0, 0, 0};
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        auto const viewport = D3D12_VIEWPORT{.Width = 8, .Height = 8, .MaxDepth = 1};
        auto const scissor = D3D12_RECT{.right = 8, .bottom = 8};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->SetGraphicsRootSignature(root.Get());
        if (vertex)
        {
            list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawInstanced(3, 1, 0, 0);
        }
        else
            list->DispatchMesh(1, 1, 1);
        auto barrier = D3D12_RESOURCE_BARRIER{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        barrier.Transition = {target.Get(), 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        auto const dst = D3D12_TEXTURE_COPY_LOCATION{.pResource = readback.Get(),
                                                     .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                     .PlacedFootprint = {0, {DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 256}}};
        auto const src = D3D12_TEXTURE_COPY_LOCATION{.pResource = target.Get(),
                                                     .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                                     .SubresourceIndex = 0};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier.Transition = {target.Get(), 0, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET};
        list->ResourceBarrier(1, &barrier);
        CHECK_HR(list->Close());
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        CHECK_HR(queue->Signal(fence.Get(), ++fence_value));
        auto const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(fence_value, event);
        WaitForSingleObject(event, 10000);
        CloseHandle(event);

        unsigned char* p = nullptr;
        readback->Map(0, nullptr, reinterpret_cast<void**>(&p));
        auto const* px = p + 3 * 256 + 3 * 4;
        auto const landed = px[1] == 255;
        all_landed = all_landed && landed;
        std::printf("%c: pixel (3, 3) = %u %u %u %u%s%s\n", *step, px[0], px[1], px[2], px[3],
                    restoring ? ", built from the blob" : "", landed ? "" : "  <- DREW NOTHING");
        readback->Unmap(0, nullptr);
    }
    return all_landed ? 0 : 1;
}
