// Creates one compute pipeline from a DXIL container, on the first adapter of a PCI vendor.
// usage: repro.exe <container> [--vendor <hex id, default 1002 (AMD)>]
// It exits 0 when the pipeline was created, 3 when creation failed with an HRESULT, and 2 without an adapter or device.
// A crash inside the driver ends the process with its exception code instead, which run.py reads.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv)
{
    if (argc < 2)
        return std::printf("usage: repro.exe <container> [--vendor <hex>]\n"), 2;
    auto vendor = 0x1002u;
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--vendor"))
            vendor = unsigned(std::strtoul(argv[i + 1], nullptr, 16));

    std::ifstream file(argv[1], std::ios::binary);
    auto const code = std::vector<char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (desc.VendorId == vendor)
        {
            std::printf("adapter: %ls\n", desc.Description);
            break;
        }
    }
    if (!adapter)
        return std::printf("no adapter of vendor %04x\n", vendor), 2;

    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
        return std::printf("no device\n"), 2;

    // t0 and u1 as root descriptors, which is all the shader binds
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor = {0, 0};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {1, 0};
    D3D12_ROOT_SIGNATURE_DESC const root_desc = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)))
        return std::printf("no root signature\n"), 2;
    ComPtr<ID3D12RootSignature> root;
    device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root.Get();
    desc.CS = {code.data(), code.size()};
    std::printf("creating the pipeline from %zu bytes\n", code.size());
    std::fflush(stdout);
    ComPtr<ID3D12PipelineState> pso;
    HRESULT const hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    std::printf("CreateComputePipelineState: hr=0x%08X\n", unsigned(hr));
    return SUCCEEDED(hr) ? 0 : 3;
}
