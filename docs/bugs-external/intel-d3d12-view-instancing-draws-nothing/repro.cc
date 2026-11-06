// D3D12 alone: one draw of two views into a two-layer target, then how many texels of each layer it wrote.
// Usage: repro.exe vertex.dxil pixel.dxil [--warp] [--views N]
// Exits 0 when every view wrote, 1 when one did not, 2 when the run proves nothing.
// The debug layer is always on, and anything it says is printed, because `run.py` refuses a run it has a complaint about.

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT width = 4;
constexpr UINT height = 2;
constexpr UINT row_pitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
constexpr FLOAT clear_value[] = {-1, -1, -1, -1};
constexpr auto format = DXGI_FORMAT_R32G32B32A32_FLOAT;

ComPtr<ID3D12InfoQueue> debug_messages;

void print_debug_messages()
{
    if (!debug_messages)
        return;
    for (UINT64 i = 0; i < debug_messages->GetNumStoredMessages(); ++i)
    {
        auto size = SIZE_T(0);
        debug_messages->GetMessage(i, nullptr, &size);
        auto storage = std::vector<char>(size);
        auto* const message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        debug_messages->GetMessage(i, message, &size);
        std::printf("debug layer: %s\n", message->pDescription);
    }
}

void check(HRESULT hr, char const* what)
{
    if (SUCCEEDED(hr))
        return;
    print_debug_messages();
    std::printf("error: %s (0x%08lx)\n", what, static_cast<unsigned long>(hr));
    std::exit(2);
}

std::vector<char> read_file(char const* path)
{
    auto file = std::ifstream(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(file), {});
}

template <class T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) subobject
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value = {};
};

// view instancing can only be stated through a stream, whose omitted fields have no useful defaults
struct pipeline_stream
{
    subobject<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root_signature;
    subobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS> vs;
    subobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
    subobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY> topology;
    subobject<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> formats;
    subobject<D3D12_VIEW_INSTANCING_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING> view_instancing;
    subobject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
    subobject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
    subobject<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sample_mask;
    subobject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample_desc;
    subobject<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth_stencil;
};
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: repro.exe vertex.dxil pixel.dxil [--warp] [--views N]\n");
        return 2;
    }
    auto warp = false;
    auto views = UINT(2);
    for (auto i = 3; i < argc; ++i)
    {
        warp = warp || std::strcmp(argv[i], "--warp") == 0;
        if (std::strcmp(argv[i], "--views") == 0 && i + 1 < argc)
            views = UINT(std::atoi(argv[i + 1]));
    }
    auto const vs = read_file(argv[1]);
    auto const ps = read_file(argv[2]);

    ComPtr<ID3D12Debug> layer;
    check(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)), "no debug layer; Windows' Graphics Tools feature provides it");
    layer->EnableDebugLayer();

    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> adapter;
    if (warp)
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
    else
        check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)),
              "EnumAdapterByGpuPreference");
    auto adapter_desc = DXGI_ADAPTER_DESC1{};
    adapter->GetDesc1(&adapter_desc);

    ComPtr<ID3D12Device2> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    check(device.As(&debug_messages), "ID3D12InfoQueue");
    auto options = D3D12_FEATURE_DATA_D3D12_OPTIONS3{};
    check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options)), "CheckFeatureSupport");
    std::printf("adapter: %ls, view instancing tier %d\n", adapter_desc.Description, int(options.ViewInstancingTier));
    std::fflush(stdout);

    auto const root_desc = D3D12_ROOT_SIGNATURE_DESC{};
    ComPtr<ID3DBlob> root_blob;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, nullptr), "serialize");
    ComPtr<ID3D12RootSignature> root_signature;
    check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                      IID_PPV_ARGS(&root_signature)),
          "CreateRootSignature");

    D3D12_VIEW_INSTANCE_LOCATION const locations[] = {{0, 0}, {0, 1}};
    auto stream = pipeline_stream{};
    stream.root_signature.value = root_signature.Get();
    stream.vs.value = {vs.data(), vs.size()};
    stream.ps.value = {ps.data(), ps.size()};
    stream.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    stream.formats.value.NumRenderTargets = 1;
    stream.formats.value.RTFormats[0] = format;
    stream.view_instancing.value = {views, locations, D3D12_VIEW_INSTANCING_FLAG_NONE};
    stream.rasterizer.value.FillMode = D3D12_FILL_MODE_SOLID;
    stream.rasterizer.value.CullMode = D3D12_CULL_MODE_NONE;
    stream.rasterizer.value.DepthClipEnable = TRUE;
    stream.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    stream.sample_mask.value = UINT_MAX;
    stream.sample_desc.value = {1, 0};
    auto const stream_desc = D3D12_PIPELINE_STATE_STREAM_DESC{sizeof(stream), &stream};
    ComPtr<ID3D12PipelineState> pipeline;
    check(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pipeline)), "CreatePipelineState");

    auto const default_heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_DEFAULT};
    auto const readback_heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_READBACK};
    auto const target_desc = D3D12_RESOURCE_DESC{.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                                 .Width = width,
                                                 .Height = height,
                                                 .DepthOrArraySize = 2,
                                                 .MipLevels = 1,
                                                 .Format = format,
                                                 .SampleDesc = {1, 0},
                                                 .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    auto clear = D3D12_CLEAR_VALUE{};
    clear.Format = format;
    std::memcpy(clear.Color, clear_value, sizeof(clear_value));
    ComPtr<ID3D12Resource> target;
    check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &target_desc,
                                          D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target)),
          "create target");

    auto const buffer_desc = D3D12_RESOURCE_DESC{.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
                                                 .Width = row_pitch * height * 2,
                                                 .Height = 1,
                                                 .DepthOrArraySize = 1,
                                                 .MipLevels = 1,
                                                 .SampleDesc = {1, 0},
                                                 .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ComPtr<ID3D12Resource> readback;
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
          "create readback");

    auto const heap_desc = D3D12_DESCRIPTOR_HEAP_DESC{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV, .NumDescriptors = 1};
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
    auto const rtv = heap->GetCPUDescriptorHandleForHeapStart();
    // value-initialized, then filled: a designated initializer leaves the rest of the union indeterminate
    auto rtv_desc = D3D12_RENDER_TARGET_VIEW_DESC{};
    rtv_desc.Format = format;
    rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtv_desc.Texture2DArray.ArraySize = 2;
    device->CreateRenderTargetView(target.Get(), &rtv_desc, rtv);

    auto const queue_desc = D3D12_COMMAND_QUEUE_DESC{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    ComPtr<ID3D12GraphicsCommandList1> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
                                    IID_PPV_ARGS(&list)),
          "CreateCommandList");

    list->ClearRenderTargetView(rtv, clear_value, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    auto const viewport = D3D12_VIEWPORT{0, 0, FLOAT(width), FLOAT(height), 0, 1};
    auto const scissor = D3D12_RECT{0, 0, LONG(width), LONG(height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(root_signature.Get());
    list->SetViewInstanceMask((1u << views) - 1u);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    list->DrawInstanced(4, 1, 0, 0);

    auto barrier = D3D12_RESOURCE_BARRIER{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {target.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    for (UINT layer_index = 0; layer_index < 2; ++layer_index)
    {
        auto source = D3D12_TEXTURE_COPY_LOCATION{};
        source.pResource = target.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = layer_index;
        auto dest = D3D12_TEXTURE_COPY_LOCATION{};
        dest.pResource = readback.Get();
        dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dest.PlacedFootprint = {row_pitch * height * layer_index, {format, width, height, 1, row_pitch}};
        list->CopyTextureRegion(&dest, 0, 0, 0, &source, nullptr);
    }
    check(list->Close(), "Close");

    ID3D12CommandList* const lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    auto const event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    check(queue->Signal(fence.Get(), 1), "Signal");
    check(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion");
    if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
        check(E_FAIL, "the queue never finished");
    print_debug_messages();

    void* mapped = nullptr;
    check(readback->Map(0, nullptr, &mapped), "Map");
    auto const written = [&](UINT layer_index)
    {
        auto count = 0;
        auto const* const base = static_cast<char*>(mapped) + row_pitch * height * layer_index;
        for (UINT y = 0; y < height; ++y)
            for (UINT x = 0; x < width; ++x)
            {
                auto value = 0.0f;
                std::memcpy(&value, base + y * row_pitch + x * 16, sizeof(value));
                count += value != clear_value[0] ? 1 : 0;
            }
        return count;
    };
    auto const left = written(0);
    auto const right = views < 2 ? left : written(1);
    std::printf("written: %d %d\n", left, right);
    std::fflush(stdout);
    return left > 0 && right > 0 ? 0 : 1;
}
