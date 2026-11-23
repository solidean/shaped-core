// D3D12 alone: a graphics pipeline created through a stream, its cached blob saved, then the pipeline created again
// from that blob.
// Usage: repro.exe vertex.dxil pixel.dxil --save <blob> | --load <blob> | --both
//        [--adapter <substring>|warp] [--views N] [--debug] [--no-view-instancing] [--scribble]
// --save creates the pipeline fresh and writes its blob, --load creates it from a blob an earlier process wrote,
// and --both does the two in one process.
// --scribble, with --both, restates the view locations from another address and overwrites the first one before the restore.
// Prints `restored` when the creation from the blob returned, whatever its HRESULT; a driver crash prints nothing more.
// Exits 0 on success, 3 when the driver refused the blob (a legal answer), 2 when the run proves nothing.

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <climits>
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
    debug_messages->ClearStoredMessages();
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
    subobject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
    subobject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
    subobject<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sample_mask;
    subobject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample_desc;
    subobject<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth_stencil;
    subobject<D3D12_CACHED_PIPELINE_STATE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO> cached;
    subobject<D3D12_VIEW_INSTANCING_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING> view_instancing;
};

// the same stream without the view-instancing subobject, the control
struct plain_stream
{
    subobject<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root_signature;
    subobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS> vs;
    subobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
    subobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY> topology;
    subobject<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> formats;
    subobject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
    subobject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
    subobject<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sample_mask;
    subobject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample_desc;
    subobject<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth_stencil;
    subobject<D3D12_CACHED_PIPELINE_STATE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO> cached;
};

template <class Stream>
void fill(Stream& stream, ID3D12RootSignature* root, std::vector<char> const& vs, std::vector<char> const& ps)
{
    stream.root_signature.value = root;
    stream.vs.value = {vs.data(), vs.size()};
    stream.ps.value = {ps.data(), ps.size()};
    stream.topology.value = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    stream.formats.value.NumRenderTargets = 1;
    stream.formats.value.RTFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    stream.rasterizer.value.FillMode = D3D12_FILL_MODE_SOLID;
    stream.rasterizer.value.CullMode = D3D12_CULL_MODE_NONE;
    stream.rasterizer.value.DepthClipEnable = TRUE;
    stream.blend.value.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    stream.sample_mask.value = UINT_MAX;
    stream.sample_desc.value = {1, 0};
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::printf("usage: repro.exe vertex.dxil pixel.dxil --save <blob> | --load <blob> | --both [options]\n");
        return 2;
    }
    auto save_path = std::string();
    auto load_path = std::string();
    auto both = false;
    auto adapter_match = std::string();
    auto views = UINT(2);
    auto debug = false;
    auto view_instancing = true;
    auto scribble = false;
    for (auto i = 3; i < argc; ++i)
    {
        auto const arg = std::string(argv[i]);
        auto const has_next = i + 1 < argc;
        if (arg == "--save" && has_next)
            save_path = argv[++i];
        else if (arg == "--load" && has_next)
            load_path = argv[++i];
        else if (arg == "--both")
            both = true;
        else if (arg == "--adapter" && has_next)
            adapter_match = argv[++i];
        else if (arg == "--views" && has_next)
            views = UINT(std::atoi(argv[++i]));
        else if (arg == "--debug")
            debug = true;
        else if (arg == "--no-view-instancing")
            view_instancing = false;
        else if (arg == "--scribble")
            scribble = true;
    }
    auto const vs = read_file(argv[1]);
    auto const ps = read_file(argv[2]);

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
        // the first hardware adapter whose name holds the substring, case-insensitively; the high-performance one without
        for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                             IID_PPV_ARGS(&adapter))
                         != DXGI_ERROR_NOT_FOUND;
             ++i)
        {
            auto desc = DXGI_ADAPTER_DESC1{};
            adapter->GetDesc1(&desc);
            auto name = std::wstring(desc.Description);
            auto match = std::wstring(adapter_match.begin(), adapter_match.end());
            for (auto& c : name)
                c = wchar_t(towlower(c));
            for (auto& c : match)
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

    ComPtr<ID3D12Device2> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
    if (debug)
        check(device.As(&debug_messages), "ID3D12InfoQueue");
    auto options = D3D12_FEATURE_DATA_D3D12_OPTIONS3{};
    check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof(options)), "CheckFeatureSupport");
    auto const v = driver.QuadPart;
    std::printf("adapter: %ls, driver %d.%d.%d.%d, view instancing tier %d\n", adapter_desc.Description,
                int(v >> 48), int((v >> 32) & 0xffff), int((v >> 16) & 0xffff), int(v & 0xffff),
                int(options.ViewInstancingTier));
    std::fflush(stdout);

    auto const root_desc = D3D12_ROOT_SIGNATURE_DESC{};
    ComPtr<ID3DBlob> root_blob;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, nullptr), "serialize");
    ComPtr<ID3D12RootSignature> root_signature;
    check(device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(),
                                      IID_PPV_ARGS(&root_signature)),
          "CreateRootSignature");

    auto first_locations = std::vector<D3D12_VIEW_INSTANCE_LOCATION>{{0, 0}, {0, 1}, {0, 2}, {0, 3}};
    auto second_locations = first_locations;
    D3D12_VIEW_INSTANCE_LOCATION const* locations = first_locations.data();
    auto const create = [&](std::vector<char> const& blob, ComPtr<ID3D12PipelineState>& out)
    {
        auto const cached = D3D12_CACHED_PIPELINE_STATE{blob.empty() ? nullptr : blob.data(), blob.size()};
        if (view_instancing)
        {
            auto stream = pipeline_stream{};
            fill(stream, root_signature.Get(), vs, ps);
            stream.cached.value = cached;
            stream.view_instancing.value = {views, locations, D3D12_VIEW_INSTANCING_FLAG_NONE};
            auto const desc = D3D12_PIPELINE_STATE_STREAM_DESC{sizeof(stream), &stream};
            return device->CreatePipelineState(&desc, IID_PPV_ARGS(&out));
        }
        auto stream = plain_stream{};
        fill(stream, root_signature.Get(), vs, ps);
        stream.cached.value = cached;
        auto const desc = D3D12_PIPELINE_STATE_STREAM_DESC{sizeof(stream), &stream};
        return device->CreatePipelineState(&desc, IID_PPV_ARGS(&out));
    };

    auto blob = std::vector<char>();
    if (!load_path.empty())
    {
        blob = read_file(load_path.c_str());
        if (blob.empty())
            check(E_FAIL, "an empty blob file");
    }
    else
    {
        ComPtr<ID3D12PipelineState> fresh;
        check(create({}, fresh), "CreatePipelineState, fresh");
        ComPtr<ID3DBlob> cached;
        check(fresh->GetCachedBlob(&cached), "GetCachedBlob");
        auto const* const bytes = static_cast<char const*>(cached->GetBufferPointer());
        blob.assign(bytes, bytes + cached->GetBufferSize());
        std::printf("fresh: a blob of %zu bytes\n", blob.size());
        std::fflush(stdout);
        if (!save_path.empty())
        {
            auto file = std::ofstream(save_path, std::ios::binary);
            file.write(blob.data(), std::streamsize(blob.size()));
            print_debug_messages();
            return 0;
        }
        if (!both)
            return 0;
        if (scribble)
        {
            // the restore states the same locations from another address, and the first address holds garbage
            std::memset(first_locations.data(), 0xFF, first_locations.size() * sizeof(first_locations[0]));
            locations = second_locations.data();
        }
    }

    ComPtr<ID3D12PipelineState> restored;
    auto const hr = create(blob, restored);
    print_debug_messages();
    std::printf("restored: 0x%08lx\n", static_cast<unsigned long>(hr));
    std::fflush(stdout);
    return SUCCEEDED(hr) ? 0 : 3;
}
