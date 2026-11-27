// Creates a compute pipeline from a DXIL container on the first adapter of a PCI vendor, AMD's unless argv[2] names one.
// Its root signature is a root SRV at t0 and a root UAV at u1, which flat.hlsl and split.hlsl bind.
// Exits 0 when the pipeline is created and 1 when creation fails; a crash in the driver ends the process instead.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <d3d12.h>
#include <dxgi1_6.h>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    unsigned const vendor = argc > 2 ? strtoul(argv[2], nullptr, 16) : 0x1002;
    FILE* f = nullptr;
    if (fopen_s(&f, argv[1], "rb") != 0 || !f)
        return 3;
    std::vector<char> blob;
    char buffer[4096];
    for (size_t n; (n = fread(buffer, 1, sizeof buffer, f)) > 0;)
        blob.insert(blob.end(), buffer, buffer + n);
    fclose(f);

    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return 4;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (d.VendorId == vendor)
        {
            printf("adapter: %ls\n", d.Description);
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    if (!adapter)
        return 4;
    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
        return 5;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor = {0, 0};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {1, 0};
    D3D12_ROOT_SIGNATURE_DESC const rs = {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob* serialized = nullptr;
    ID3DBlob* error = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error)))
        return 6;
    ID3D12RootSignature* root = nullptr;
    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&root))))
        return 7;

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = root;
    desc.CS = {blob.data(), blob.size()};
    ID3D12PipelineState* pso = nullptr;
    HRESULT const hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
    printf("CreateComputePipelineState: 0x%08lx\n", static_cast<unsigned long>(hr));
    return SUCCEEDED(hr) ? 0 : 1;
}
