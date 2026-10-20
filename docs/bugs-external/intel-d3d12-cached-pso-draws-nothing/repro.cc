// Draws one quad with vertex.hlsl and pixel.hlsl into eight render targets, and reports whether the draw landed.
//
// Standalone: D3D12, DXGI and the C++ standard library, nothing else.
// The shaders are DXIL that run.py compiled with the Windows SDK's dxc.
//
//     repro.exe <vs.cso> <ps.cso> [--warp] [--full-layout] [--debug]
//
// --full-layout  the root signature shaped-core used: the seven bindless texture ranges, a third table and root constants on top
// --debug        the D3D12 debug layer with GPU-based validation, printing every message it reports
//
// Exit codes: 0 the quad was drawn, 1 the targets still hold their clear value, 2 setup failed.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace
{
constexpr UINT size = 4;
constexpr float cleared = -1.0f;

// pixel.hlsl's bindless table: seven texture ranges, then 4096 raw buffers at t4656 in space 1.
constexpr UINT bindless_counts[] = {64, 32, 4096, 256, 64, 16, 128, 4096};
constexpr UINT buffers_base = 4656;

// The G-buffer pixel.hlsl writes, in SV_Target order; only 0 (color) and 7 (primary depth) are read back.
constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                   DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16_FLOAT,
                                   DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_R32_FLOAT};

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

D3D12_RESOURCE_DESC texture_desc(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags)
{
    auto d = D3D12_RESOURCE_DESC{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = size;
    d.Height = size;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes)
{
    auto d = D3D12_RESOURCE_DESC{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: repro.exe <vs.cso> <ps.cso> [--warp] [--full-layout]\n");
        return 2;
    }
    auto const vs = read_file(argv[1]);
    auto const ps = read_file(argv[2]);
    auto warp = false;
    auto full_layout = false;
    auto debug = false;
    for (auto i = 3; i < argc; ++i)
    {
        warp = warp || std::strcmp(argv[i], "--warp") == 0;
        full_layout = full_layout || std::strcmp(argv[i], "--full-layout") == 0;
        debug = debug || std::strcmp(argv[i], "--debug") == 0;
    }
    if (debug)
    {
        ComPtr<ID3D12Debug1> layer;
        CHECK_HR(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)));
        layer->EnableDebugLayer();
        layer->SetEnableGPUBasedValidation(TRUE);
    }
    if (vs.empty() || ps.empty())
    {
        std::printf("could not read the shaders\n");
        return 2;
    }

    ComPtr<IDXGIFactory6> factory;
    CHECK_HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    if (warp)
        CHECK_HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    else
        CHECK_HR(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device;
    CHECK_HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));

    // ---- Root signature: table 0 holds pixel.hlsl's six structured buffers, table 1 its bindless arrays ----------

    auto group0 = std::vector<D3D12_DESCRIPTOR_RANGE>();
    for (UINT i = 0; i < 6; ++i)
        group0.push_back({D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i, 0, i});
    auto group1 = std::vector<D3D12_DESCRIPTOR_RANGE>();
    UINT bindless_total = 0;
    for (UINT i = 0; i < 8; ++i)
    {
        auto const is_buffers = i == 7;
        if (full_layout || is_buffers)
            group1.push_back({D3D12_DESCRIPTOR_RANGE_TYPE_SRV, bindless_counts[i], is_buffers ? buffers_base : bindless_total,
                              1, full_layout ? bindless_total : 0});
        bindless_total += bindless_counts[i];
    }
    auto group2 = std::vector<D3D12_DESCRIPTOR_RANGE>();
    for (UINT i = 0; i < 2; ++i)
        group2.push_back({D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, i, 2, i});

    auto params = std::vector<D3D12_ROOT_PARAMETER>();
    auto const add_table = [&](std::vector<D3D12_DESCRIPTOR_RANGE> const& ranges)
    {
        auto p = D3D12_ROOT_PARAMETER{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p.DescriptorTable.NumDescriptorRanges = UINT(ranges.size());
        p.DescriptorTable.pDescriptorRanges = ranges.data();
        params.push_back(p);
    };
    add_table(group0);
    add_table(group1);
    if (full_layout)
    {
        add_table(group2);
        auto p = D3D12_ROOT_PARAMETER{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p.Constants = {0, 9, 4};
        params.push_back(p);
    }

    auto rs_desc = D3D12_ROOT_SIGNATURE_DESC{};
    rs_desc.NumParameters = UINT(params.size());
    rs_desc.pParameters = params.data();
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs_blob;
    ComPtr<ID3DBlob> rs_error;
    CHECK_HR(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &rs_blob, &rs_error));
    ComPtr<ID3D12RootSignature> root_signature;
    CHECK_HR(device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                         IID_PPV_ARGS(&root_signature)));

    // ---- Pipeline: eight targets and a reversed depth buffer, as shaped-core's raster pass has them --------------

    auto pso = D3D12_GRAPHICS_PIPELINE_STATE_DESC{};
    pso.pRootSignature = root_signature.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {ps.data(), ps.size()};
    pso.BlendState.IndependentBlendEnable = TRUE;
    for (auto& rt : pso.BlendState.RenderTarget)
    {
        rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 8;
    for (auto i = 0; i < 8; ++i)
        pso.RTVFormats[i] = formats[i];
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    auto const t0 = std::chrono::steady_clock::now();
    CHECK_HR(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)));
    auto const create_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // ---- Inputs: frame constants, one instance record, the quad's six vertices, a parameter block ----------------

    auto const default_heap = D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_DEFAULT};
    auto const upload_heap = D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_UPLOAD};
    auto const readback_heap = D3D12_HEAP_PROPERTIES{D3D12_HEAP_TYPE_READBACK};

    auto const make_upload = [&](void const* data, UINT64 bytes, ComPtr<ID3D12Resource>& out) -> bool
    {
        auto const d = buffer_desc((bytes + 255) & ~UINT64(255));
        if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&out))))
            return false;
        void* mapped = nullptr;
        if (FAILED(out->Map(0, nullptr, &mapped)))
            return false;
        std::memset(mapped, 0, size_t(d.Width));
        std::memcpy(mapped, data, size_t(bytes));
        out->Unmap(0, nullptr);
        return true;
    };

    // frame_constants: a camera (position, forward, right_scaled, up_scaled, each padded to a float4) twice, then near_plane, light_count, decal_count, pixel_spread, size and padding.
    // No lights and no decals.
    float frame[40] = {};
    float const camera[16] = {0, 0, -3.4f, 0, 0, 0, 1, 0, 0.41421356f, 0, 0, 0, 0, 0.41421356f, 0, 0};
    std::memcpy(frame, camera, sizeof(camera));
    std::memcpy(frame + 16, camera, sizeof(camera));
    frame[32] = 0.1f;
    frame[35] = 0.01f;
    frame[36] = float(size);
    frame[37] = float(size);
    // instance_record: its parameter block is bindless buffer 2 and its vertices buffer 1, not indexed, placed at the origin.
    std::uint32_t instance[28] = {};
    instance[0] = 2;
    instance[2] = 1;
    instance[5] = ~0u;
    float const rows[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    std::memcpy(instance + 16, rows, sizeof(rows));
    float const quad[18] = {-2, -2, 0, 2, -2, 0, 2, 2, 0, -2, -2, 0, 2, 2, 0, -2, 2, 0};
    float param_block[64] = {};
    for (auto& v : param_block)
        v = 1.0f;

    ComPtr<ID3D12Resource> frame_buffer, instance_buffer, vertex_buffer, param_buffer;
    if (!make_upload(frame, sizeof(frame), frame_buffer) || !make_upload(instance, sizeof(instance), instance_buffer)
        || !make_upload(quad, sizeof(quad), vertex_buffer) || !make_upload(param_block, sizeof(param_block), param_buffer))
    {
        std::printf("could not create the inputs\n");
        return 2;
    }

    // One shader-visible heap: table 0 at 0, table 1 right after it; every descriptor null but the four inputs.
    UINT const table1_size = full_layout ? bindless_total : bindless_counts[7];
    UINT const table1_start = 6;
    UINT const buffers_start = table1_start + (full_layout ? bindless_total - bindless_counts[7] : 0);
    UINT const table2_start = table1_start + table1_size;
    UINT const heap_size = table2_start + 2;
    ComPtr<ID3D12DescriptorHeap> heap;
    {
        auto d = D3D12_DESCRIPTOR_HEAP_DESC{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, heap_size,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        CHECK_HR(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&heap)));
    }
    auto const stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto const cpu_at = [&](UINT i)
    { return D3D12_CPU_DESCRIPTOR_HANDLE{heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * stride}; };
    auto const gpu_at = [&](UINT i)
    { return D3D12_GPU_DESCRIPTOR_HANDLE{heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(i) * stride}; };
    auto const raw_view = [&](ID3D12Resource* r, UINT at)
    {
        auto v = D3D12_SHADER_RESOURCE_VIEW_DESC{};
        v.Format = DXGI_FORMAT_R32_TYPELESS;
        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Buffer.NumElements = r != nullptr ? 64 : 0;
        v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(r, &v, cpu_at(at));
    };
    auto const structured_view = [&](ID3D12Resource* r, UINT element_size, UINT at)
    {
        auto v = D3D12_SHADER_RESOURCE_VIEW_DESC{};
        v.Format = DXGI_FORMAT_UNKNOWN;
        v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Buffer.NumElements = 1;
        v.Buffer.StructureByteStride = element_size;
        device->CreateShaderResourceView(r, &v, cpu_at(at));
    };
    for (UINT i = 0; i < heap_size; ++i)
        raw_view(nullptr, i);
    structured_view(frame_buffer.Get(), sizeof(frame), 0);
    structured_view(instance_buffer.Get(), sizeof(instance), 1);
    raw_view(vertex_buffer.Get(), buffers_start + 1);
    raw_view(param_buffer.Get(), buffers_start + 2);

    // ---- Targets ---------------------------------------------------------------------------------------------------

    ComPtr<ID3D12Resource> targets[8];
    for (auto i = 0; i < 8; ++i)
    {
        auto const d = texture_desc(formats[i], D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        CHECK_HR(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                 nullptr, IID_PPV_ARGS(&targets[i])));
    }
    ComPtr<ID3D12Resource> depth;
    {
        auto const d = texture_desc(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        CHECK_HR(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                 nullptr, IID_PPV_ARGS(&depth)));
    }
    ComPtr<ID3D12Resource> readback;
    {
        auto const d = buffer_desc(2 * 256 * size);
        CHECK_HR(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&readback)));
    }
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    {
        auto r = D3D12_DESCRIPTOR_HEAP_DESC{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8};
        CHECK_HR(device->CreateDescriptorHeap(&r, IID_PPV_ARGS(&rtv_heap)));
        auto d = D3D12_DESCRIPTOR_HEAP_DESC{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1};
        CHECK_HR(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&dsv_heap)));
    }
    auto const rtv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8];
    for (auto i = 0; i < 8; ++i)
    {
        rtvs[i] = {rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * rtv_stride};
        device->CreateRenderTargetView(targets[i].Get(), nullptr, rtvs[i]);
    }
    auto const dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateDepthStencilView(depth.Get(), nullptr, dsv);

    // ---- Draw ------------------------------------------------------------------------------------------------------

    ComPtr<ID3D12CommandQueue> queue;
    {
        auto d = D3D12_COMMAND_QUEUE_DESC{D3D12_COMMAND_LIST_TYPE_DIRECT};
        CHECK_HR(device->CreateCommandQueue(&d, IID_PPV_ARGS(&queue)));
    }
    ComPtr<ID3D12CommandAllocator> allocator;
    CHECK_HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;
    CHECK_HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(),
                                       IID_PPV_ARGS(&list)));

    float const clear[] = {cleared, cleared, cleared, cleared};
    for (auto const& rtv : rtvs)
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
    list->OMSetRenderTargets(8, rtvs, FALSE, &dsv);
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(root_signature.Get());
    list->SetGraphicsRootDescriptorTable(0, gpu_at(0));
    list->SetGraphicsRootDescriptorTable(1, gpu_at(table1_start));
    if (full_layout)
    {
        list->SetGraphicsRootDescriptorTable(2, gpu_at(table2_start));
        UINT const constants[4] = {};
        list->SetGraphicsRoot32BitConstants(3, 4, constants, 0);
    }
    auto const viewport = D3D12_VIEWPORT{0, 0, float(size), float(size), 0, 1};
    auto const scissor = D3D12_RECT{0, 0, LONG(size), LONG(size)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(6, 1, 0, 0);

    for (auto const t : {0, 7})
    {
        auto b = D3D12_RESOURCE_BARRIER{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = targets[t].Get();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        auto dst = D3D12_TEXTURE_COPY_LOCATION{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = t == 0 ? 0 : 256 * size;
        dst.PlacedFootprint.Footprint = {formats[t], size, size, 1, 256};
        auto src = D3D12_TEXTURE_COPY_LOCATION{};
        src.pResource = targets[t].Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    CHECK_HR(list->Close());

    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    CHECK_HR(queue->Signal(fence.Get(), 1));
    auto* const done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK_HR(fence->SetEventOnCompletion(1, done));
    WaitForSingleObject(done, INFINITE);
    CloseHandle(done);
    CHECK_HR(device->GetDeviceRemovedReason());

    // ---- Verdict: the centre pixel of color and of primary depth ----------------------------------------------------

    float color[4] = {};
    float primary_depth = 0;
    {
        void* mapped = nullptr;
        CHECK_HR(readback->Map(0, nullptr, &mapped));
        auto const* bytes = static_cast<unsigned char const*>(mapped);
        std::memcpy(color, bytes + 256 * (size / 2) + 16 * (size / 2), sizeof(color));
        std::memcpy(&primary_depth, bytes + 256 * size + 256 * (size / 2) + 4 * (size / 2), sizeof(primary_depth));
        readback->Unmap(0, nullptr);
    }
    if (ComPtr<ID3D12InfoQueue> info; debug && SUCCEEDED(device.As(&info)))
    {
        // Errors in full; the rest are the startup notice and "no optimized clear value", so they are only counted.
        auto errors = 0;
        auto others = 0;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T length = 0;
            info->GetMessage(i, nullptr, &length);
            auto storage = std::vector<char>(length);
            auto* const message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            info->GetMessage(i, message, &length);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            {
                ++errors;
                std::printf("debug layer error: %s\n", message->pDescription);
            }
            else
                ++others;
        }
        std::printf("debug layer: %d error(s), %d other message(s); ", errors, others);
    }

    auto const drawn = primary_depth != cleared;
    std::printf("%-10s color %g %g %g, primary depth %g, pipeline created in %.1f ms\n", drawn ? "drawn:" : "NOT DRAWN:",
                double(color[0]), double(color[1]), double(color[2]), double(primary_depth), create_ms);
    return drawn ? 0 : 1;
}
