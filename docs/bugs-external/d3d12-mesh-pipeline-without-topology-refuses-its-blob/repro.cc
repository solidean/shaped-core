// Builds one pipeline, takes its cached blob, and builds it again from that blob, on each adapter and on WARP.
//
// Standalone: D3D12, DXGI and the C++ standard library, nothing else.
// The shaders are DXIL that run.py compiled with the Windows SDK's dxc.
//
//     repro.exe <mesh.cso> <vertex.cso>
//
// Three pipelines, each through a pipeline-state stream under a root signature of no parameter:
// the vertex stage with a triangle topology, the mesh stage alone, and the mesh stage with a triangle topology.
// Prints one line an adapter, the HRESULT of each restore: 0x00000000 where the blob was taken.
// Exit codes: 1 when some adapter refuses the bare mesh stage's blob and takes the other two, 0 when every adapter
// takes all three, 2 for anything else.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
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

using root_signature = subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, ID3D12RootSignature*>;
using topology = subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE>;
using cached = subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO, D3D12_CACHED_PIPELINE_STATE>;

struct vertex_stream
{
    root_signature root;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE> vs;
    topology topo;
    cached blob;
};

struct mesh_stream
{
    root_signature root;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    cached blob;
};

struct mesh_topology_stream
{
    root_signature root;
    subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, D3D12_SHADER_BYTECODE> ms;
    topology topo;
    cached blob;
};

/// Builds `stream`, then builds it again from the first pipeline's blob, and returns the second build's HRESULT.
template <class Stream>
HRESULT restore(ID3D12Device2* device, Stream stream)
{
    auto const desc = D3D12_PIPELINE_STATE_STREAM_DESC{.SizeInBytes = sizeof(stream), .pPipelineStateSubobjectStream = &stream};
    ComPtr<ID3D12PipelineState> first;
    if (auto const hr = device->CreatePipelineState(&desc, IID_PPV_ARGS(&first)); FAILED(hr))
        return hr;
    ComPtr<ID3DBlob> blob;
    if (auto const hr = first->GetCachedBlob(&blob); FAILED(hr))
        return hr;
    stream.blob.value = {blob->GetBufferPointer(), blob->GetBufferSize()};
    ComPtr<ID3D12PipelineState> second;
    return device->CreatePipelineState(&desc, IID_PPV_ARGS(&second));
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: repro.exe <mesh.cso> <vertex.cso>\n");
        return 2;
    }
    auto const mesh = read_file(argv[1]);
    auto const vertex = read_file(argv[2]);
    if (mesh.empty() || vertex.empty())
    {
        std::printf("could not read the shaders\n");
        return 2;
    }

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return 2;

    auto reproduced = false;
    auto all_taken = true;
    auto ran = 0;
    // -1 is WARP, then every hardware adapter
    for (auto i = -1;; ++i)
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (i < 0)
            factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        else if (factory->EnumAdapters1(UINT(i), &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        auto desc = DXGI_ADAPTER_DESC1{};
        adapter->GetDesc1(&desc);
        if (i >= 0 && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            continue;
        ComPtr<ID3D12Device2> device;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
            continue;
        auto options7 = D3D12_FEATURE_DATA_D3D12_OPTIONS7{};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)))
            || options7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED)
            continue;

        auto const root_desc = D3D12_ROOT_SIGNATURE_DESC{};
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3D12RootSignature> root;
        if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr))
            || FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                  IID_PPV_ARGS(&root))))
            return 2;

        auto const ms = D3D12_SHADER_BYTECODE{mesh.data(), mesh.size()};
        auto v = vertex_stream{};
        v.root.value = root.Get();
        v.vs.value = {vertex.data(), vertex.size()};
        v.topo.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        auto m = mesh_stream{};
        m.root.value = root.Get();
        m.ms.value = ms;
        auto t = mesh_topology_stream{};
        t.root.value = root.Get();
        t.ms.value = ms;
        t.topo.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        auto const vertex_hr = restore(device.Get(), v);
        auto const mesh_hr = restore(device.Get(), m);
        auto const topology_hr = restore(device.Get(), t);
        std::printf("%-36ls vertex 0x%08lx  mesh 0x%08lx  mesh with topology 0x%08lx\n", desc.Description,
                    static_cast<unsigned long>(vertex_hr), static_cast<unsigned long>(mesh_hr),
                    static_cast<unsigned long>(topology_hr));
        ++ran;
        reproduced = reproduced || (FAILED(mesh_hr) && SUCCEEDED(vertex_hr) && SUCCEEDED(topology_hr));
        all_taken = all_taken && SUCCEEDED(vertex_hr) && SUCCEEDED(mesh_hr) && SUCCEEDED(topology_hr);
    }
    if (ran == 0)
        return 2;
    if (reproduced)
        return 1;
    return all_taken ? 0 : 2;
}
