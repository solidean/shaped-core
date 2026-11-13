// Two ray-tracing pipelines from one HLSL file on one D3D12 device, compiled with and without payload access qualifiers.
// Each traces an 8 x 4 grid of rays and writes one word per ray, which must be 10.
//
// Standalone: D3D12, DXGI, the Agility SDK's headers and runtime, and DXC's dxcompiler.dll, plus the C++ standard library.
//
// usage: repro.exe --agility <dir of D3D12Core.dll> --dxc <dir of dxcompiler.dll> --shader <file.hlsl>
//                  [--qualifiers mixed|reversed|both|neither] [--separate-devices] [-D NAME[=VALUE]]... [--vendor <hex>]
// `mixed` (the default) compiles the second pipeline with -disable-payload-qualifiers, `reversed` the first.
// `both` and `neither` compile the two alike.
// `--separate-devices` gives each pipeline a device of its own, which the Agility SDK's device factory makes per call.
// Prints each pipeline's words.
// Exits 0 when every word of both is 10, 1 when only the second pipeline's are wrong, and 2 for anything else.

// clang-format off
// must precede d3d12.h: it defines the Agility headers' CLSID_D3D12SDKConfiguration, which the system d3d12.lib lacks
#include <initguid.h>
// clang-format on
#include <d3d12.h>
#include <dxcapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace
{
#define CHECK_HR(expr)                                                                   \
    do                                                                                   \
    {                                                                                    \
        if (HRESULT hr_ = (expr); FAILED(hr_))                                           \
        {                                                                                \
            std::printf("FAILED %s: 0x%08lx\n", #expr, static_cast<unsigned long>(hr_)); \
            std::exit(2);                                                                \
        }                                                                                \
    } while (false)

constexpr UINT width = 8;
constexpr UINT height = 4;
constexpr UINT expected = 10;

// one triangle at z = 0 that covers every ray of the grid
float const triangle[] = {-1.0f, -1.0f, 0.0f, 40.0f, -1.0f, 0.0f, -1.0f, 40.0f, 0.0f};

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

/// A device with its queue, the one-triangle scene, and the root signature: t0 and u0 as root descriptors.
struct gpu
{
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value = 0;
    ComPtr<ID3D12RootSignature> root;
    D3D12_GPU_VIRTUAL_ADDRESS tlas = 0;
    std::vector<ComPtr<ID3D12Resource>> keep;
    std::vector<ComPtr<ID3D12StateObject>> pipelines; // every pipeline stays alive, as in the process that hit it

    ID3D12Resource* buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
    {
        auto const props = D3D12_HEAP_PROPERTIES{.Type = heap};
        auto const desc = buffer_desc(size, flags);
        ComPtr<ID3D12Resource> r;
        CHECK_HR(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&r)));
        keep.push_back(r);
        return r.Get();
    }

    ID3D12Resource* upload(void const* data, UINT64 size)
    {
        auto* const r = buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = nullptr;
        r->Map(0, nullptr, &p);
        std::memcpy(p, data, size);
        r->Unmap(0, nullptr);
        return r;
    }

    ComPtr<ID3D12GraphicsCommandList4> new_list()
    {
        ComPtr<ID3D12CommandAllocator> allocator;
        CHECK_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList4> list;
        CHECK_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        return list;
    }

    void submit_and_wait(ID3D12GraphicsCommandList4* list)
    {
        CHECK_HR(list->Close());
        ID3D12CommandList* const lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        CHECK_HR(queue->Signal(fence.Get(), ++fence_value));
        CHECK_HR(fence->SetEventOnCompletion(fence_value, nullptr));
    }

    ID3D12Resource* build(ID3D12GraphicsCommandList4* list, D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS const& inputs)
    {
        auto info = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
        auto const uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        auto* const result = buffer(info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav,
                                    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        auto* const scratch = buffer(info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_COMMON);
        auto build = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC{};
        build.Inputs = inputs;
        build.DestAccelerationStructureData = result->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
        auto barrier = D3D12_RESOURCE_BARRIER{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV};
        barrier.UAV.pResource = result;
        list->ResourceBarrier(1, &barrier);
        return result;
    }
};

void create_gpu(gpu& g, ID3D12DeviceFactory* factory, IDXGIAdapter1* adapter)
{
    CHECK_HR(factory->CreateDevice(adapter, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&g.device)));
    auto model = D3D12_FEATURE_DATA_SHADER_MODEL{D3D_SHADER_MODEL_6_9};
    if (FAILED(g.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)))
        || model.HighestShaderModel < D3D_SHADER_MODEL_6_9)
    {
        std::printf("no shader model 6.9\n");
        std::exit(2);
    }
    auto const queue_desc = D3D12_COMMAND_QUEUE_DESC{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK_HR(g.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&g.queue)));
    CHECK_HR(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)));

    auto const list = g.new_list();
    auto* const vertices = g.upload(triangle, sizeof(triangle));
    auto geometry = D3D12_RAYTRACING_GEOMETRY_DESC{};
    geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexCount = 3;
    geometry.Triangles.VertexBuffer = {.StartAddress = vertices->GetGPUVirtualAddress(), .StrideInBytes = 12};
    auto blas_inputs = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS{};
    blas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blas_inputs.NumDescs = 1;
    blas_inputs.pGeometryDescs = &geometry;
    auto* const blas = g.build(list.Get(), blas_inputs);
    auto instance = D3D12_RAYTRACING_INSTANCE_DESC{};
    instance.Transform[0][0] = instance.Transform[1][1] = instance.Transform[2][2] = 1.0f;
    instance.InstanceMask = 0xff;
    instance.AccelerationStructure = blas->GetGPUVirtualAddress();
    auto* const instances = g.upload(&instance, sizeof(instance));
    auto tlas_inputs = D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS{};
    tlas_inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlas_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlas_inputs.NumDescs = 1;
    tlas_inputs.InstanceDescs = instances->GetGPUVirtualAddress();
    g.tlas = g.build(list.Get(), tlas_inputs)->GetGPUVirtualAddress();
    g.submit_and_wait(list.Get());

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    auto const root_desc = D3D12_ROOT_SIGNATURE_DESC{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    CHECK_HR(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error));
    CHECK_HR(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root)));
}

/// The shader file compiled as one lib_6_9 library, with or without payload access qualifiers.
ComPtr<IDxcBlob> compile(DxcCreateInstanceProc create, std::string const& source, std::vector<std::wstring> const& defines,
                         bool qualifiers)
{
    ComPtr<IDxcCompiler3> compiler;
    CHECK_HR(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)));
    std::vector<LPCWSTR> args = {L"-T", L"lib_6_9", L"-O3"};
    for (auto const& d : defines)
        args.insert(args.end(), {L"-D", d.c_str()});
    if (!qualifiers)
        args.push_back(L"-disable-payload-qualifiers");
    auto const buffer = DxcBuffer{source.data(), source.size(), DXC_CP_UTF8};
    ComPtr<IDxcResult> compiled;
    CHECK_HR(compiler->Compile(&buffer, args.data(), UINT32(args.size()), nullptr, IID_PPV_ARGS(&compiled)));
    HRESULT status = S_OK;
    compiled->GetStatus(&status);
    if (FAILED(status))
    {
        ComPtr<IDxcBlobUtf8> errors;
        compiled->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        std::printf("compile failed:\n%s\n", errors ? errors->GetStringPointer() : "");
        std::exit(2);
    }
    ComPtr<IDxcBlob> library;
    CHECK_HR(compiled->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&library), nullptr));
    return library;
}

/// The words one pipeline from `library` writes on `g`.
std::vector<unsigned> trace(gpu& g, IDxcBlob* library)
{
    auto lib_desc = D3D12_DXIL_LIBRARY_DESC{};
    lib_desc.DXILLibrary = {library->GetBufferPointer(), library->GetBufferSize()};
    auto hits = D3D12_HIT_GROUP_DESC{L"hits", D3D12_HIT_GROUP_TYPE_TRIANGLES, nullptr, L"ch", nullptr};
    auto shader_config = D3D12_RAYTRACING_SHADER_CONFIG{.MaxPayloadSizeInBytes = 4, .MaxAttributeSizeInBytes = 8};
    auto pipeline_config = D3D12_RAYTRACING_PIPELINE_CONFIG{.MaxTraceRecursionDepth = 1};
    auto global_root = D3D12_GLOBAL_ROOT_SIGNATURE{g.root.Get()};
    D3D12_STATE_SUBOBJECT subobjects[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib_desc},
        {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hits},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global_root},
    };
    auto const so_desc = D3D12_STATE_OBJECT_DESC{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, UINT(std::size(subobjects)),
                                                 subobjects};
    ComPtr<ID3D12StateObject> pipeline;
    CHECK_HR(g.device->CreateStateObject(&so_desc, IID_PPV_ARGS(&pipeline)));
    g.pipelines.push_back(pipeline);
    ComPtr<ID3D12StateObjectProperties> properties;
    CHECK_HR(pipeline.As(&properties));

    // the shader table: the raygen, the miss and the hit group, each at a table's alignment
    constexpr auto align = UINT64(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    constexpr auto record = UINT64(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    unsigned char table[3 * align] = {};
    wchar_t const* const exports[] = {L"rg", L"ms", L"hits"};
    for (int t = 0; t < 3; ++t)
        std::memcpy(table + t * align, properties->GetShaderIdentifier(exports[t]), record);
    auto const base = g.upload(table, sizeof(table))->GetGPUVirtualAddress();

    constexpr auto bytes = UINT64(width * height * 4);
    auto* const words = g.buffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto* const readback = g.buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto const list = g.new_list();
    list->SetComputeRootSignature(g.root.Get());
    list->SetComputeRootShaderResourceView(0, g.tlas);
    list->SetComputeRootUnorderedAccessView(1, words->GetGPUVirtualAddress());
    list->SetPipelineState1(pipeline.Get());
    auto dispatch = D3D12_DISPATCH_RAYS_DESC{};
    dispatch.RayGenerationShaderRecord = {base, record};
    dispatch.MissShaderTable = {base + align, record, record};
    dispatch.HitGroupTable = {base + 2 * align, record, record};
    dispatch.Width = width;
    dispatch.Height = height;
    dispatch.Depth = 1;
    list->DispatchRays(&dispatch);
    auto barrier = D3D12_RESOURCE_BARRIER{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    barrier.Transition = {words, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    list->CopyResource(readback, words);
    g.submit_and_wait(list.Get());

    unsigned* seen = nullptr;
    auto const range = D3D12_RANGE{0, bytes};
    if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&seen))) || !seen)
    {
        std::printf("device removed: 0x%08X\n", unsigned(g.device->GetDeviceRemovedReason()));
        std::exit(2);
    }
    auto out = std::vector<unsigned>(seen, seen + width * height);
    readback->Unmap(0, nullptr);
    return out;
}

bool report(char const* label, std::vector<unsigned> const& words)
{
    auto ok = true;
    std::printf("%s:", label);
    for (UINT i = 0; i < width * height; ++i)
    {
        std::printf("%s%u", i % width == 0 ? " |" : " ", words[i]);
        ok = ok && words[i] == expected;
    }
    std::printf("\n");
    return ok;
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char const* agility = nullptr;
    char const* dxc_dir = nullptr;
    char const* shader_path = nullptr;
    std::string qualifiers = "mixed";
    auto separate = false;
    auto vendor = 0x10deu;
    std::vector<std::wstring> defines;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "--agility") && i + 1 < argc)
            agility = argv[++i];
        else if (!std::strcmp(argv[i], "--dxc") && i + 1 < argc)
            dxc_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--shader") && i + 1 < argc)
            shader_path = argv[++i];
        else if (!std::strcmp(argv[i], "--qualifiers") && i + 1 < argc)
            qualifiers = argv[++i];
        else if (!std::strcmp(argv[i], "--separate-devices"))
            separate = true;
        else if (!std::strcmp(argv[i], "--vendor") && i + 1 < argc)
            vendor = unsigned(std::strtoul(argv[++i], nullptr, 16));
        else if (!std::strcmp(argv[i], "-D") && i + 1 < argc)
        {
            std::string const d = argv[++i];
            defines.emplace_back(d.begin(), d.end());
        }
    if (!agility || !dxc_dir || !shader_path)
        return std::printf("usage: repro.exe --agility <dir> --dxc <dir> --shader <file> "
                           "[--qualifiers mixed|reversed|both|neither] [--separate-devices] [-D NAME[=VALUE]]... "
                           "[--vendor <hex>]\n"),
               2;

    auto const dll = LoadLibraryA((std::string(dxc_dir) + "\\dxcompiler.dll").c_str());
    if (!dll)
        return std::printf("no dxcompiler.dll in %s\n", dxc_dir), 2;
    auto const create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dll, "DxcCreateInstance"));
    std::ifstream file(shader_path, std::ios::binary);
    auto const source = std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    auto const first = compile(create, source, defines, qualifiers != "neither" && qualifiers != "reversed");
    auto const second = compile(create, source, defines, qualifiers == "both" || qualifiers == "reversed");

    ComPtr<IDXGIFactory6> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        auto desc = DXGI_ADAPTER_DESC1{};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == vendor)
            break;
    }
    if (!adapter)
        return std::printf("no adapter of vendor %04x\n", vendor), 2;
    auto adesc = DXGI_ADAPTER_DESC1{};
    adapter->GetDesc1(&adesc);
    std::printf("adapter: %ls\n", adesc.Description);

    // the Agility runtime, since shader model 6.9 needs it
    ComPtr<ID3D12SDKConfiguration1> configuration;
    CHECK_HR(D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&configuration)));
    ComPtr<ID3D12DeviceFactory> device_factory;
    CHECK_HR(configuration->CreateDeviceFactory(D3D12_SDK_VERSION, agility, IID_PPV_ARGS(&device_factory)));

    static gpu a, b;
    create_gpu(a, device_factory.Get(), adapter.Get());
    if (separate)
        create_gpu(b, device_factory.Get(), adapter.Get());
    auto& second_gpu = separate ? b : a;

    auto const first_ok = report("first pipeline ", trace(a, first.Get()));
    auto const second_ok = report("second pipeline", trace(second_gpu, second.Get()));
    if (first_ok && second_ok)
        return 0;
    return first_ok ? 1 : 2;
}
