// Runs one mesh-shaded draw of shader.hlsl's mesh stage alone, and prints whether the device survived it.
//
// Standalone: D3D12, DXGI and the C++ standard library, nothing else.
// The shader is DXIL that run.py compiled with the Windows SDK's dxc.
//
//     repro.exe <mesh.cso> (--adapter <substring> | --warp) [--root-constant]
//
// The pipeline is the mesh stage and a root signature: of no parameter, or with --root-constant of one 32-bit constant
// no shader reads, which nothing sets.
// No pixel stage, no target, no resource: the mesh stage writes one vertex and no triangle.
// Prints `kept | <adapter>` or `removed 0x<reason> | <adapter>`.
// Exit codes: 0 kept, 1 removed, 2 setup failed, 3 no hardware adapter's name holds the substring.

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
};
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: repro.exe <mesh.cso> (--adapter <substring> | --warp) [--root-constant]\n");
        return 2;
    }
    auto const mesh = read_file(argv[1]);
    if (mesh.empty())
    {
        std::printf("could not read the shader\n");
        return 2;
    }
    auto with_constant = false;
    for (int a = 2; a < argc; ++a)
        with_constant = with_constant || std::strcmp(argv[a], "--root-constant") == 0;

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

    ComPtr<ID3D12Device2> device;
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    auto options7 = D3D12_FEATURE_DATA_D3D12_OPTIONS7{};
    CHECK_HR(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)));
    if (options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
    {
        std::printf("no mesh shaders | %ls\n", desc.Description);
        return 2;
    }

    auto constant = D3D12_ROOT_PARAMETER{.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                                         .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL};
    constant.Constants = {.ShaderRegister = 0, .RegisterSpace = 100, .Num32BitValues = 1};
    auto const root_desc
        = D3D12_ROOT_SIGNATURE_DESC{.NumParameters = with_constant ? 1u : 0u, .pParameters = &constant};
    ComPtr<ID3DBlob> serialized;
    CHECK_HR(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr));
    ComPtr<ID3D12RootSignature> root;
    CHECK_HR(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                         IID_PPV_ARGS(&root)));

    auto stream = mesh_stream{};
    stream.root_signature.value = root.Get();
    stream.ms.value = {mesh.data(), mesh.size()};
    auto const stream_desc
        = D3D12_PIPELINE_STATE_STREAM_DESC{.SizeInBytes = sizeof(stream), .pPipelineStateSubobjectStream = &stream};
    ComPtr<ID3D12PipelineState> pso;
    CHECK_HR(device->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&pso)));

    auto const queue_desc = D3D12_COMMAND_QUEUE_DESC{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    ComPtr<ID3D12CommandQueue> queue;
    CHECK_HR(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    CHECK_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList6> list;
    CHECK_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pso.Get(),
                                       IID_PPV_ARGS(&list)));
    auto const viewport = D3D12_VIEWPORT{.Width = 8.0f, .Height = 8.0f, .MaxDepth = 1.0f};
    auto const scissor = D3D12_RECT{.right = 8, .bottom = 8};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(root.Get());
    list->DispatchMesh(1, 1, 1);
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

    auto const removed = device->GetDeviceRemovedReason();
    if (FAILED(removed))
    {
        std::printf("removed 0x%08lx | %ls\n", static_cast<unsigned long>(removed), desc.Description);
        return 1;
    }
    std::printf("kept | %ls\n", desc.Description);
    return 0;
}
