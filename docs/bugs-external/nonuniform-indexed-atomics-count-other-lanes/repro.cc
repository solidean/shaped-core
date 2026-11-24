// D3D12 alone: one dispatch of 64 threads, each updating the buffer of an array of two that its parity picks.
// Usage: repro.exe shader.dxil [--adapter <substring>|warp] [--debug]
// Prints the two words of each buffer; expected `even 32 0, odd 32 1`.
// Exits 0 when every word is the expected one, 1 when one is not, 2 when the run proves nothing.

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT threads = 64;
constexpr UINT results_words = threads * 2;

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
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: repro.exe shader.dxil [--adapter <substring>|warp] [--debug]\n");
        return 2;
    }
    auto adapter_match = std::string();
    auto debug = false;
    for (auto i = 2; i < argc; ++i)
    {
        auto const arg = std::string(argv[i]);
        if (arg == "--adapter" && i + 1 < argc)
            adapter_match = argv[++i];
        else if (arg == "--debug")
            debug = true;
    }
    auto const code = read_file(argv[1]);
    if (code.empty())
        check(E_FAIL, "no shader");

    if (debug)
    {
        ComPtr<ID3D12Debug> layer;
        check(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)), "no debug layer; Windows' Graphics Tools feature provides it");
        layer->EnableDebugLayer();
    }

    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    ComPtr<IDXGIAdapter1> adapter;
    if (adapter_match == "warp")
        check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "EnumWarpAdapter");
    else
    {
        // the first hardware adapter whose name holds the substring, case-insensitively
        auto match = std::wstring(adapter_match.begin(), adapter_match.end());
        for (auto& c : match)
            c = wchar_t(towlower(c));
        for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                             IID_PPV_ARGS(&adapter))
                         != DXGI_ERROR_NOT_FOUND;
             ++i)
        {
            auto desc = DXGI_ADAPTER_DESC1{};
            adapter->GetDesc1(&desc);
            auto name = std::wstring(desc.Description);
            for (auto& c : name)
                c = wchar_t(towlower(c));
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && name.find(match) != std::wstring::npos)
                break;
            adapter.Reset();
        }
        if (!adapter)
            check(E_FAIL, "no adapter matches --adapter");
    }
    auto adapter_desc = DXGI_ADAPTER_DESC1{};
    adapter->GetDesc1(&adapter_desc);
    auto driver = LARGE_INTEGER{};
    adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);

    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    if (debug)
        check(device.As(&debug_messages), "ID3D12InfoQueue");
    auto options1 = D3D12_FEATURE_DATA_D3D12_OPTIONS1{};
    check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1)), "CheckFeatureSupport");
    auto const v = driver.QuadPart;
    std::printf("adapter: %ls, driver %d.%d.%d.%d, wave %u-%u\n", adapter_desc.Description, int(v >> 48),
                int((v >> 32) & 0xffff), int((v >> 16) & 0xffff), int(v & 0xffff), options1.WaveLaneCountMin,
                options1.WaveLaneCountMax);
    std::fflush(stdout);

    // one descriptor table of three UAVs: the two-element array at u0 and `results` at u2
    auto const range = D3D12_DESCRIPTOR_RANGE{.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                                              .NumDescriptors = 3,
                                              .BaseShaderRegister = 0,
                                              .RegisterSpace = 0,
                                              .OffsetInDescriptorsFromTableStart = 0};
    auto param = D3D12_ROOT_PARAMETER{};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable = {1, &range};
    auto const root_desc = D3D12_ROOT_SIGNATURE_DESC{1, &param, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> root_blob;
    ComPtr<ID3DBlob> root_error;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &root_error), "serialize");
    ComPtr<ID3D12RootSignature> root;
    check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&root)),
          "CreateRootSignature");

    auto pso_desc = D3D12_COMPUTE_PIPELINE_STATE_DESC{};
    pso_desc.pRootSignature = root.Get();
    pso_desc.CS = {code.data(), code.size()};
    ComPtr<ID3D12PipelineState> pso;
    check(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)), "CreateComputePipelineState");

    // even, odd and results, each its own committed buffer, filled from one upload buffer
    auto const on_gpu = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_DEFAULT};
    auto const upload_heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_UPLOAD};
    auto const readback_heap = D3D12_HEAP_PROPERTIES{.Type = D3D12_HEAP_TYPE_READBACK};
    UINT const words[3] = {2, 2, results_words};
    ComPtr<ID3D12Resource> buffers[3];
    for (auto i = 0; i < 3; ++i)
    {
        auto const desc = buffer_desc(words[i] * 4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        check(device->CreateCommittedResource(&on_gpu, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, IID_PPV_ARGS(&buffers[i])),
              "create buffer");
    }
    auto const staging_size = UINT64(4 * 4);
    auto const staging_desc = buffer_desc(staging_size, D3D12_RESOURCE_FLAG_NONE);
    ComPtr<ID3D12Resource> upload;
    check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &staging_desc,
                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)),
          "create upload");
    UINT* initial = nullptr;
    check(upload->Map(0, nullptr, reinterpret_cast<void**>(&initial)), "Map upload");
    UINT const start[4] = {0, 0xFFFFFFFFu, 0, 0xFFFFFFFFu};
    std::memcpy(initial, start, sizeof(start));
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12Resource> readback;
    check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &staging_desc,
                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
          "create readback");

    auto const heap_desc = D3D12_DESCRIPTOR_HEAP_DESC{.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                                      .NumDescriptors = 3,
                                                      .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ComPtr<ID3D12DescriptorHeap> heap;
    check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
    auto const increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (auto i = 0; i < 3; ++i)
    {
        auto uav = D3D12_UNORDERED_ACCESS_VIEW_DESC{};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = words[i];
        uav.Buffer.StructureByteStride = 4;
        auto at = heap->GetCPUDescriptorHandleForHeapStart();
        at.ptr += SIZE_T(i) * increment;
        device->CreateUnorderedAccessView(buffers[i].Get(), nullptr, &uav, at);
    }

    auto const queue_desc = D3D12_COMMAND_QUEUE_DESC{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                    IID_PPV_ARGS(&list)),
          "CreateCommandList");

    auto const transition = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        auto b = D3D12_RESOURCE_BARRIER{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        list->ResourceBarrier(1, &b);
    };
    // buffers in COMMON promote to COPY_DEST on their first copy
    list->CopyBufferRegion(buffers[0].Get(), 0, upload.Get(), 0, 8);
    list->CopyBufferRegion(buffers[1].Get(), 0, upload.Get(), 8, 8);
    for (auto i = 0; i < 2; ++i)
        transition(buffers[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(buffers[2].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* const heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch(1, 1, 1);

    for (auto i = 0; i < 2; ++i)
        transition(buffers[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, buffers[0].Get(), 0, 8);
    list->CopyBufferRegion(readback.Get(), 8, buffers[1].Get(), 0, 8);
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
    check(device->GetDeviceRemovedReason(), "device removed");
    print_debug_messages();

    UINT* seen = nullptr;
    check(readback->Map(0, nullptr, reinterpret_cast<void**>(&seen)), "Map readback");
    std::printf("even %u %u, odd %u %u\n", seen[0], seen[1], seen[2], seen[3]);
    std::fflush(stdout);
    auto const ok = seen[0] == threads / 2 && seen[1] == 0 && seen[2] == threads / 2 && seen[3] == 1;
    return ok ? 0 : 1;
}
